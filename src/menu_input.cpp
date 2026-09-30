// The menu's keyboard and mouse: see menu_input.h.
//
// SetWindowSubclass and RemoveWindowSubclass want the window's own thread. The menu opens on the game's render
// thread, which in most games is also the window's thread, and then they are called directly. When it is not, the
// window procedure is pointed at a small function of ours (SetWindowLongPtr works across threads of one process)
// whose first message, on the window's thread, puts the original back and installs the subclass the same way; a
// posted message ends the same way. Everything ImGui is told goes through the menu's lock, like the frame.
//
// The mouse: a game in play keeps the cursor hidden and often held still (clipped to a point, or put back to the
// centre every frame), so the cursor's position says nothing. While the menu is open the raw mouse data (WM_INPUT)
// is registered to the game's window for us, and it moves a pointer of our own, told to ImGui as the mouse
// messages the backend understands; ImGui draws that pointer. The game's own registration, if any, is put back
// when the menu closes. Until raw data comes the window's mouse messages serve, as in the example.

#include "menu_input.h"

#include <commctrl.h>
#include <windowsx.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "log.h"
#include "menu.h"
#include "overlay_dx12.h"

// From the Win32 backend, as its header says to declare it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef RI_MOUSE_HWHEEL
#define RI_MOUSE_HWHEEL 0x0800
#endif

namespace
{
constexpr UINT_PTR kSubclassId = 0xBA4A;
constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageMouse = 0x02;
constexpr USHORT kUsageKeyboard = 0x06;

HWND g_window = nullptr;
std::atomic<bool> g_capture { false }; // the menu is open: ImGui sees the messages
std::atomic<bool> g_attached { false };
std::atomic<bool> g_pending { false }; // our stand-in procedure waits for its first message
WNDPROC g_original = nullptr;
bool g_unicode = true;
UINT g_detachMessage = 0;
const char* g_state = "not attached";

// Raw input: what the game had registered for the mouse and the keyboard, ours in its place while capturing.
bool g_registered = false;
RAWINPUTDEVICE g_saved[2];
unsigned g_savedCount = 0;

// Our pointer, in client pixels, packed so that the thread that draws can reset it while the window's thread
// moves it. Counts since the open, for the log.
std::atomic<uint64_t> g_pointer { 0 };
std::atomic<bool> g_rawMouse { false }; // raw mouse data came since the open: the pointer follows it
std::atomic<unsigned> g_rawMouseEvents { 0 };
std::atomic<unsigned> g_mouseMessages { 0 };
std::atomic<unsigned> g_keyMessages { 0 };

POINT LoadPointer()
{
    const uint64_t packed = g_pointer.load(std::memory_order_relaxed);
    return POINT { LONG(int32_t(uint32_t(packed))), LONG(int32_t(uint32_t(packed >> 32))) };
}

void StorePointer(POINT p)
{
    g_pointer.store(uint64_t(uint32_t(p.x)) | (uint64_t(uint32_t(p.y)) << 32), std::memory_order_relaxed);
}

bool OnWindowThread(HWND window) { return GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId(); }

bool InFront()
{
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

bool MouseMessage(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSELEAVE ||
           message == WM_NCMOUSEMOVE || message == WM_SETCURSOR;
}

bool KeyboardMessage(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_UNICHAR;
}

// A release the game still gets, so that nothing it saw pressed before the menu opened stays pressed.
bool ReleaseMessage(UINT message)
{
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP ||
           message == WM_KEYUP || message == WM_SYSKEYUP;
}

// The frame is not always the client area's size (a scaled window): the mouse is told in frame pixels.
LPARAM ScaleMouse(HWND window, LPARAM lParam)
{
    unsigned width = 0, height = 0;
    RECT client;
    if (!OverlayFrameSize(&width, &height) || width == 0 || height == 0 || !GetClientRect(window, &client))
        return lParam;
    const LONG clientWidth = client.right - client.left;
    const LONG clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0 || (LONG(width) == clientWidth && LONG(height) == clientHeight))
        return lParam;
    const int x = GET_X_LPARAM(lParam) * int(width) / int(clientWidth);
    const int y = GET_Y_LPARAM(lParam) * int(height) / int(clientHeight);
    return MAKELPARAM(x, y);
}

// ---------------------------------------------------------------------------------------------------------------
// Raw input.

const char* UsageName(USHORT usage) { return usage == kUsageMouse ? "mouse" : "keyboard"; }

void RegisterRaw(HWND window)
{
    if (g_registered)
        return;
    g_savedCount = 0;
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)); // -1 and the count, by design
    if (count > 0 && count <= 64)
    {
        RAWINPUTDEVICE devices[64];
        const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
        for (UINT i = 0; got != UINT(-1) && i < got && g_savedCount < 2; ++i)
            if (devices[i].usUsagePage == kUsagePageGeneric &&
                (devices[i].usUsage == kUsageMouse || devices[i].usUsage == kUsageKeyboard))
                g_saved[g_savedCount++] = devices[i];
    }
    char had[160] = "no registration of its own";
    if (g_savedCount != 0)
    {
        int at = 0;
        for (unsigned i = 0; i < g_savedCount; ++i)
            at += snprintf(had + at, sizeof had - size_t(at), "%s%s -> window %p, flags 0x%lX", i != 0 ? ", " : "",
                           UsageName(g_saved[i].usUsage), g_saved[i].hwndTarget, g_saved[i].dwFlags);
    }
    RAWINPUTDEVICE ours[2] = {};
    for (unsigned i = 0; i < 2; ++i)
    {
        ours[i].usUsagePage = kUsagePageGeneric;
        ours[i].usUsage = i == 0 ? kUsageMouse : kUsageKeyboard;
        ours[i].dwFlags = RIDEV_INPUTSINK;
        ours[i].hwndTarget = window;
    }
    g_registered = RegisterRawInputDevices(ours, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    if (g_registered)
        Log("menu: input: raw mouse and keyboard registered to the window; the game had %s", had);
    else
        Log("menu: input: RegisterRawInputDevices failed (error %lu); the game had %s", GetLastError(), had);
}

void RestoreRaw()
{
    if (!g_registered)
        return;
    g_registered = false;
    bool ok = true;
    for (unsigned u = 0; u < 2; ++u)
    {
        const USHORT usage = u == 0 ? kUsageMouse : kUsageKeyboard;
        RAWINPUTDEVICE back = {};
        back.usUsagePage = kUsagePageGeneric;
        back.usUsage = usage;
        back.dwFlags = RIDEV_REMOVE; // the game had none: ours goes
        for (unsigned i = 0; i < g_savedCount; ++i)
            if (g_saved[i].usUsage == usage)
                back = g_saved[i];
        if (!RegisterRawInputDevices(&back, 1, sizeof(RAWINPUTDEVICE)))
        {
            ok = false;
            Log("menu: input: the game's %s registration could not be put back (error %lu)", UsageName(usage),
                GetLastError());
        }
    }
    if (ok)
        Log("menu: input: the game's raw input registration is back");
}

// The raw data behind a WM_INPUT, on the window's thread. Mouse data moves the pointer and becomes ImGui's mouse
// messages; keyboard data stays here but for the releases; anything else is the game's. Returns whether the game
// gets the message.
bool OnRawInput(HWND window, LPARAM lParam)
{
    alignas(8) unsigned char buffer[256];
    UINT size = sizeof buffer;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return true; // larger than a mouse's or a keyboard's: some other device's, the game's
    const RAWINPUT& raw = *reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw.header.dwType == RIM_TYPEKEYBOARD)
        return (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0;
    if (raw.header.dwType != RIM_TYPEMOUSE)
        return true;
    g_rawMouseEvents.fetch_add(1, std::memory_order_relaxed);
    if (!InFront())
        return false; // the user is elsewhere: not the menu's, not the game's

    const RAWMOUSE& m = raw.data.mouse;
    POINT pointer = LoadPointer();
    bool moved = false;
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
    {
        // A tablet or a remote desktop: 0..65535 over the desktop.
        const bool whole = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int left = whole ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int top = whole ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int width = GetSystemMetrics(whole ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int height = GetSystemMetrics(whole ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        POINT p = { left + MulDiv(m.lLastX, width, 65535), top + MulDiv(m.lLastY, height, 65535) };
        if (ScreenToClient(window, &p))
        {
            moved = p.x != pointer.x || p.y != pointer.y;
            pointer = p;
        }
    }
    else if (m.lLastX != 0 || m.lLastY != 0)
    {
        pointer.x += m.lLastX;
        pointer.y += m.lLastY;
        moved = true;
    }
    RECT client;
    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
    {
        pointer.x = pointer.x < 0 ? 0 : pointer.x >= client.right ? client.right - 1 : pointer.x;
        pointer.y = pointer.y < 0 ? 0 : pointer.y >= client.bottom ? client.bottom - 1 : pointer.y;
    }
    StorePointer(pointer);
    const bool first = !g_rawMouse.exchange(true, std::memory_order_acq_rel);
    if (first)
        Log("menu: input: raw mouse data came, the pointer follows it");

    if (!MenuTryLock(first || m.usButtonFlags != 0 ? 50 : 5))
        return false; // a long frame holds the lock: a movement lost is nothing, the next one tells the position
    const LPARAM at = ScaleMouse(window, MAKELPARAM(pointer.x, pointer.y));
    if (moved || first)
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEMOVE, 0, at);
    struct Button
    {
        USHORT down, up;
        UINT downMessage, upMessage;
        WPARAM wParam;
    };
    static const Button kButtons[] = {
        { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN, WM_LBUTTONUP, 0 },
        { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN, WM_RBUTTONUP, 0 },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, 0 },
        { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1) },
        { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2) },
    };
    for (const Button& b : kButtons)
    {
        if ((m.usButtonFlags & b.down) != 0)
            ImGui_ImplWin32_WndProcHandler(window, b.downMessage, b.wParam, at);
        if ((m.usButtonFlags & b.up) != 0)
            ImGui_ImplWin32_WndProcHandler(window, b.upMessage, b.wParam, at);
    }
    if ((m.usButtonFlags & RI_MOUSE_WHEEL) != 0)
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEWHEEL, MAKEWPARAM(0, SHORT(m.usButtonData)), at);
    if ((m.usButtonFlags & RI_MOUSE_HWHEEL) != 0)
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEHWHEEL, MAKEWPARAM(0, SHORT(m.usButtonData)), at);
    MenuUnlock();
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The subclass.

LRESULT CALLBACK SubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    if (message == g_detachMessage || message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, &SubclassProc, id);
        if (window == g_window)
        {
            g_attached.store(false, std::memory_order_release);
            g_state = message == WM_NCDESTROY ? "the window went" : "removed";
        }
        return message == WM_NCDESTROY ? DefSubclassProc(window, message, wParam, lParam) : 0;
    }
    if (!g_capture.load(std::memory_order_acquire) || window != g_window)
        return DefSubclassProc(window, message, wParam, lParam); // closed, or a window the chain left behind

    if (message == WM_INPUT)
    {
        if (OnRawInput(window, lParam))
            return DefSubclassProc(window, message, wParam, lParam);
        return DefWindowProcW(window, message, wParam, lParam); // the system's part of WM_INPUT, not the game's
    }
    const bool mouse = MouseMessage(message);
    const bool keyboard = KeyboardMessage(message);
    const bool focus = message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATEAPP;
    if (!mouse && !keyboard && !focus)
        return DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_SETCURSOR)
        return DefSubclassProc(window, message, wParam,
                               lParam); // ImGui draws its own pointer; the cursor is the game's
    const bool release = ReleaseMessage(message);
    if (mouse)
        g_mouseMessages.fetch_add(1, std::memory_order_relaxed);
    if (keyboard)
        g_keyMessages.fetch_add(1, std::memory_order_relaxed);
    if (mouse && g_rawMouse.load(std::memory_order_acquire))
        return release ? DefSubclassProc(window, message, wParam, lParam) : 0; // the raw data said it already

    // ImGui first, as in the example; then the message stays here, but for a release and the focus messages.
    if (!MenuTryLock(message == WM_MOUSEMOVE ? 5 : 50))
        return (mouse || keyboard) && !release ? 0 : DefSubclassProc(window, message, wParam, lParam);
    const LPARAM scaled = mouse && message != WM_MOUSELEAVE ? ScaleMouse(window, lParam) : lParam;
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, wParam, scaled);
    const bool typing = ImGui::GetIO().WantTextInput; // a number is being typed: Esc and the menu key belong to the box
    MenuUnlock();
    if (handled != 0)
        return handled;

    if (keyboard)
    {
        // Esc and the menu key close the menu from here, so that they work even when the game is not evaluating.
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !typing && MenuKeyPressedInWindow(unsigned(wParam)))
            return 0;
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            unsigned menuKey = 0;
            MenuKeys(&menuKey);
            if (wParam == VK_ESCAPE || (menuKey != 0 && wParam == menuKey))
                return 0;
            return DefSubclassProc(window, message, wParam, lParam);
        }
        if (message == WM_SYSKEYDOWN || message == WM_SYSCHAR || message == WM_SYSDEADCHAR)
            return DefWindowProcW(window, message, wParam, lParam); // Alt+F4 and the like keep their meaning
        return 0;
    }
    if (mouse)
        return release ? DefSubclassProc(window, message, wParam, lParam) : 0;
    return DefSubclassProc(window, message, wParam, lParam);
}

// On the window's thread, for the first message after MenuInputAttach from another thread.
LRESULT CALLBACK StandIn(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const WNDPROC original = g_original;
    if (window == g_window && g_pending.exchange(false))
    {
        const LONG_PTR now =
            g_unicode ? GetWindowLongPtrW(window, GWLP_WNDPROC) : GetWindowLongPtrA(window, GWLP_WNDPROC);
        if (now == reinterpret_cast<LONG_PTR>(&StandIn))
        {
            if (g_unicode)
                SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            else
                SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        }
        if (SetWindowSubclass(window, &SubclassProc, kSubclassId, 0))
        {
            g_attached.store(true, std::memory_order_release);
            g_state = "subclass on the window";
        }
        else
            g_state = "SetWindowSubclass failed on the window's thread";
        Log("menu: input: %s", g_state);
    }
    return g_unicode ? CallWindowProcW(original, window, message, wParam, lParam)
                     : CallWindowProcA(original, window, message, wParam, lParam);
}

// The pointer starts where the cursor is, if that is in the window, else in the middle.
void StartCapture(HWND window)
{
    g_rawMouse.store(false, std::memory_order_release);
    g_rawMouseEvents.store(0, std::memory_order_relaxed);
    g_mouseMessages.store(0, std::memory_order_relaxed);
    g_keyMessages.store(0, std::memory_order_relaxed);
    RECT client = { 0, 0, 0, 0 };
    GetClientRect(window, &client);
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    POINT start = cursor;
    if (!ScreenToClient(window, &start) || start.x < 0 || start.y < 0 || start.x >= client.right ||
        start.y >= client.bottom)
        start = POINT { client.right / 2, client.bottom / 2 };
    StorePointer(start);
    RECT clip = { 0, 0, 0, 0 };
    GetClipCursor(&clip);
    Log("menu: input: capturing (window %p, client %ldx%ld, cursor at %ld,%ld on screen, clip %ld,%ld-%ld,%ld)", window,
        client.right, client.bottom, cursor.x, cursor.y, clip.left, clip.top, clip.right, clip.bottom);
    RegisterRaw(window);
}
} // namespace

void MenuInputAttach(HWND window)
{
    if (window == nullptr)
        return;
    if (g_detachMessage == 0)
        g_detachMessage = RegisterWindowMessageW(L"BananaZero.MenuInput.Detach");
    if (g_window == window)
    {
        if (!g_capture.exchange(true, std::memory_order_acq_rel))
            StartCapture(window);
        if (g_attached.load(std::memory_order_acquire) || g_pending.load(std::memory_order_acquire))
            return;
    }
    else
    {
        // Another window: its own subclass. The one on the window before stays, inert, until that window goes.
        MenuInputRelease();
        g_attached.store(false, std::memory_order_release);
        g_pending.store(false, std::memory_order_release);
        g_window = window;
        g_capture.store(true, std::memory_order_release);
        StartCapture(window);
    }
    g_unicode = IsWindowUnicode(window) != FALSE;
    if (OnWindowThread(window))
    {
        if (SetWindowSubclass(window, &SubclassProc, kSubclassId, 0))
        {
            g_attached.store(true, std::memory_order_release);
            g_state = "subclass on the window";
        }
        else
            g_state = "SetWindowSubclass failed";
        Log("menu: input: %s (window %p, this thread)", g_state, window);
        return;
    }
    // Another thread owns the window: our stand-in takes its first message there and installs the subclass.
    g_pending.store(true, std::memory_order_release);
    const LONG_PTR previous = g_unicode ? SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StandIn))
                                        : SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StandIn));
    if (previous == 0)
    {
        g_pending.store(false, std::memory_order_release);
        g_state = "SetWindowLongPtr failed";
        Log("menu: input: %s (error %lu)", g_state, GetLastError());
        return;
    }
    g_original = reinterpret_cast<WNDPROC>(previous);
    g_state = "waiting for the window's thread";
    Log("menu: input: %s (window %p, thread %lu)", g_state, window, GetWindowThreadProcessId(window, nullptr));
    PostMessageW(window, WM_NULL, 0, 0); // something for the stand-in to see now
}

void MenuInputRelease()
{
    if (!g_capture.exchange(false, std::memory_order_acq_rel))
        return;
    RestoreRaw();
    Log("menu: input: released: %u raw mouse events, %u mouse messages, %u key messages",
        g_rawMouseEvents.load(std::memory_order_relaxed), g_mouseMessages.load(std::memory_order_relaxed),
        g_keyMessages.load(std::memory_order_relaxed));
}

void MenuInputDetach()
{
    MenuInputRelease();
    const HWND window = g_window;
    if (window == nullptr)
        return;
    if (g_pending.exchange(false))
    {
        // The stand-in never ran: put the original back ourselves (across threads, as it was set).
        const LONG_PTR now =
            g_unicode ? GetWindowLongPtrW(window, GWLP_WNDPROC) : GetWindowLongPtrA(window, GWLP_WNDPROC);
        if (now == reinterpret_cast<LONG_PTR>(&StandIn))
        {
            if (g_unicode)
                SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
            else
                SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
        }
        g_state = "removed";
    }
    if (g_attached.load(std::memory_order_acquire))
    {
        if (OnWindowThread(window))
        {
            RemoveWindowSubclass(window, &SubclassProc, kSubclassId);
            g_attached.store(false, std::memory_order_release);
            g_state = "removed";
        }
        else
            PostMessageW(window, g_detachMessage, 0, 0); // the subclass removes itself on the window's thread
    }
    Log("menu: input: detached (window %p)", window);
}

const char* MenuInputState() { return g_state; }
