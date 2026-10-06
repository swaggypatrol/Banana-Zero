// The menu's keyboard and mouse: see menu_input.h.
//
// Not a window procedure of ours: other mods keep their own on the game's window, and check that it is still the
// window's (REFramework hooks it again when it is not, on top of whatever is there, and a subclass of ours under it
// then made a loop that overflowed the window thread's stack in Resident Evil Requiem). A WH_GETMESSAGE hook on the
// window's thread sees each message as the game's loop takes it from the queue, before it is translated and
// dispatched, and turns the ones the menu keeps into WM_NULL. It goes on when the menu opens and off when it closes,
// from any thread. Everything ImGui is told goes through the menu's lock, like the frame.
//
// The mouse: a game in play keeps the cursor hidden and often held still (clipped to a point, or put back to the
// centre every frame), so the cursor's position says nothing. While the menu is open the raw mouse data (WM_INPUT)
// is registered to the game's window for us, and it moves a pointer of our own, told to ImGui as the mouse
// messages the backend understands; ImGui draws that pointer. The game's own registration, if any, is put back
// when the menu closes. Until raw data comes the window's mouse messages serve, as in the example.

#include "menu_input.h"

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
constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageMouse = 0x02;
constexpr USHORT kUsageKeyboard = 0x06;

// Set on the drawing thread before the hook goes on and cleared after it is off, read by the hook on the window's.
std::atomic<HWND> g_window { nullptr };
std::atomic<bool> g_capture { false }; // the menu is open: ImGui sees the messages
HHOOK g_hook = nullptr;
int g_front = -1; // the game in front, as ImGui was last told: -1 not yet, 0 no, 1 yes
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
// The message hook.

// Takes a message from the game: it becomes a WM_NULL, which the game's loop translates and dispatches as nothing.
void Swallow(MSG* msg)
{
    msg->message = WM_NULL;
    msg->wParam = 0;
    msg->lParam = 0;
}

// A key press that stays with the menu still makes its characters (WM_CHAR, WM_SYSCHAR), as the game's own
// TranslateMessage would have; they come back through here for ImGui.
void SwallowKey(MSG* msg)
{
    if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN)
        TranslateMessage(msg);
    Swallow(msg);
}

// One message the window's thread took from its queue while the menu is open: ImGui sees it, and the game gets it
// only when the menu does not keep it (the releases, anything not mouse or keyboard).
void OnMessage(MSG* msg)
{
    const HWND window = msg->hwnd;
    const UINT message = msg->message;
    if (message == WM_INPUT)
    {
        if (!OnRawInput(window, msg->lParam))
        {
            DefWindowProcW(window, message, msg->wParam, msg->lParam); // the system's part of WM_INPUT, not the game's
            Swallow(msg);
        }
        return;
    }
    const bool mouse = MouseMessage(message);
    const bool keyboard = KeyboardMessage(message);
    if (!mouse && !keyboard)
        return;
    const bool release = ReleaseMessage(message);
    if (mouse)
        g_mouseMessages.fetch_add(1, std::memory_order_relaxed);
    if (keyboard)
        g_keyMessages.fetch_add(1, std::memory_order_relaxed);
    if (mouse && g_rawMouse.load(std::memory_order_acquire))
    {
        if (!release)
            Swallow(msg); // the raw data said it already
        return;
    }

    // ImGui first, as in the example; then the message stays here, but for a release.
    if (!MenuTryLock(message == WM_MOUSEMOVE ? 5 : 50))
    {
        if (!release)
            SwallowKey(msg);
        return;
    }
    const LPARAM scaled = mouse && message != WM_MOUSELEAVE ? ScaleMouse(window, msg->lParam) : msg->lParam;
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, msg->wParam, scaled);
    const bool typing = ImGui::GetIO().WantTextInput; // a number is being typed: Esc and the menu key belong to the box
    MenuUnlock();
    if (handled != 0)
    {
        SwallowKey(msg);
        return;
    }

    if (keyboard)
    {
        // Esc and the menu key close the menu from here, so that they work even when the game is not evaluating.
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !typing &&
            MenuKeyPressedInWindow(unsigned(msg->wParam)))
        {
            Swallow(msg);
            return;
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            unsigned menuKey = 0;
            MenuKeys(&menuKey);
            if (msg->wParam == VK_ESCAPE || (menuKey != 0 && msg->wParam == menuKey))
                Swallow(msg);
            return;
        }
        if (message == WM_SYSKEYDOWN || message == WM_SYSCHAR || message == WM_SYSDEADCHAR)
        {
            // Alt+F4 and the like keep their meaning, without the game seeing the keys.
            if (message == WM_SYSKEYDOWN)
                TranslateMessage(msg);
            DefWindowProcW(window, message, msg->wParam, msg->lParam);
            Swallow(msg);
            return;
        }
        SwallowKey(msg);
        return;
    }
    if (!release)
        Swallow(msg);
}

// WH_GETMESSAGE on the window's thread: every message it takes from its queue (GetMessage, or PeekMessage with
// PM_REMOVE) passes here first, before the game's loop translates and dispatches it.
LRESULT CALLBACK GetMessageHook(int code, WPARAM removal, LPARAM lParam)
{
    MSG* msg = reinterpret_cast<MSG*>(lParam);
    if (code == HC_ACTION && removal == PM_REMOVE && msg != nullptr && msg->hwnd != nullptr &&
        msg->hwnd == g_window.load(std::memory_order_acquire) && g_capture.load(std::memory_order_acquire))
        OnMessage(msg);
    return CallNextHookEx(nullptr, code, removal, lParam);
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

// The focus messages are sent, not posted, so the hook never sees them: ImGui is told of a change each frame
// instead (a key held when the user switches away is then let go).
void FollowFocus()
{
    const int front = InFront() ? 1 : 0;
    if (front != g_front && ImGui::GetCurrentContext() != nullptr)
    {
        g_front = front;
        ImGui::GetIO().AddFocusEvent(front != 0);
    }
}
} // namespace

void MenuInputAttach(HWND window)
{
    if (window == nullptr)
        return;
    if (g_window.load(std::memory_order_relaxed) != window)
    {
        MenuInputRelease();
        g_window.store(window, std::memory_order_release);
    }
    FollowFocus();
    if (g_capture.load(std::memory_order_acquire))
        return;
    StartCapture(window);
    const DWORD thread = GetWindowThreadProcessId(window, nullptr);
    g_capture.store(true, std::memory_order_release); // before the hook: its first message is already the menu's
    g_hook = thread != 0 ? SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, thread) : nullptr;
    if (g_hook != nullptr)
    {
        g_state = "message hook on the window's thread";
        Log("menu: input: %s (window %p, thread %lu)", g_state, window, thread);
    }
    else
    {
        g_state = "SetWindowsHookEx failed";
        Log("menu: input: %s (window %p, thread %lu, error %lu)", g_state, window, thread, GetLastError());
    }
}

void MenuInputRelease()
{
    if (!g_capture.exchange(false, std::memory_order_acq_rel))
        return;
    if (g_hook != nullptr)
    {
        UnhookWindowsHookEx(g_hook); // a message the window's thread is handling right now still finishes
        g_hook = nullptr;
        g_state = "released";
    }
    RestoreRaw();
    Log("menu: input: released: %u raw mouse events, %u mouse messages, %u key messages",
        g_rawMouseEvents.load(std::memory_order_relaxed), g_mouseMessages.load(std::memory_order_relaxed),
        g_keyMessages.load(std::memory_order_relaxed));
}

void MenuInputDetach()
{
    MenuInputRelease();
    const HWND window = g_window.exchange(nullptr, std::memory_order_acq_rel);
    if (window == nullptr)
        return;
    Log("menu: input: detached (window %p)", window);
    g_front = -1;
    g_state = "removed";
}

const char* MenuInputState() { return g_state; }
