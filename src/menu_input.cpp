// The menu's keyboard and mouse: see menu_input.h.
//
// Not a window procedure of ours: other mods keep their own on the game's window, and check that it is still the
// window's (REFramework hooks it again when it is not, on top of whatever is there, and a subclass of ours under it
// then made a loop that overflowed the window thread's stack in Resident Evil Requiem). A WH_GETMESSAGE hook on the
// window's thread (and on its root owner's and the one in front, if they are others of the game's) sees each message
// as the game's loop takes it from the queue, before it is translated and dispatched, and turns the ones the menu
// keeps into WM_NULL. It goes on when the menu opens and off when it closes, from any thread. Everything ImGui is told
// goes through the menu's lock, like the frame.
//
// The mouse: a game in play keeps the cursor hidden and often held still (clipped to a point, or put back to the
// centre every frame), so the cursor's position says nothing. While the menu is open the raw mouse and keyboard data
// (WM_INPUT) is registered to a message-only window of ours, on a thread of ours, and it moves a pointer of our own,
// told to ImGui as the mouse messages the backend understands; ImGui draws that pointer. The game's own registration,
// if any, is put back when the menu closes. The raw data used to go to the game's window, through the hook; in Gears
// of War E-Day the hook on that window's thread saw no message at all, raw or not, in five openings, and the menu took
// no click. On a thread of ours it comes whatever the game's loop does. Until raw data comes the window's mouse
// messages serve, as in the example, and when neither comes the cursor and the buttons are read each frame.
//
// The keys: the window's key messages, through the hook, as typed (with their characters). Where the hooks see no
// mouse or key message while the raw mouse data shows the mouse moving, the keys come from the raw data instead.

#include "menu_input.h"

#include <windowsx.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "imgui.h"
#include "log.h"
#include "menu.h"
#include "overlay_dx12.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// From the Win32 backend, as its header says to declare it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// Also the backend's, not static so that others may use it, as its comment says.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

#ifndef RI_MOUSE_HWHEEL
#define RI_MOUSE_HWHEEL 0x0800
#endif

namespace
{
constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageMouse = 0x02;
constexpr USHORT kUsageKeyboard = 0x06;
constexpr unsigned kMaxHooks = 3;           // the window's thread, its root owner's, the one in front
constexpr unsigned kRegistrationEvery = 10; // frames between looks at the raw input registration
constexpr unsigned kRawMovesForKeys = 8;    // raw mouse events with no message before the keys come from raw data

// Set on the drawing thread before the hooks go on and cleared after they are off, read by the hooks and the input
// thread.
std::atomic<HWND> g_window { nullptr };
std::atomic<bool> g_capture { false }; // the menu is open: ImGui sees the input
HHOOK g_hooks[kMaxHooks] = {};
DWORD g_hookThreads[kMaxHooks] = {};
unsigned g_hookCount = 0;
int g_front = -1; // the game in front, as ImGui was last told: -1 not yet, 0 no, 1 yes
const char* g_state = "not attached";
bool g_described = false; // the game's windows and threads, logged at the first open

// The input thread: a message-only window of ours that the raw data goes to while the menu is open. Made at the first
// open, kept for the process.
HANDLE g_inputThread = nullptr;
HANDLE g_inputReady = nullptr;
std::atomic<HWND> g_inputWindow { nullptr };

// Raw input: what the game had registered for the mouse and the keyboard, ours in its place while capturing.
bool g_registered = false;
RAWINPUTDEVICE g_saved[2];
unsigned g_savedCount = 0;
unsigned g_frames = 0;  // since the open, on the drawing thread
unsigned g_retaken = 0; // times the game registered over ours since the open

// Our pointer, in client pixels, packed so that the thread that draws can reset it while the input thread moves it.
// Counts since the open, for the log.
std::atomic<uint64_t> g_pointer { 0 };
std::atomic<bool> g_rawMouse { false };    // raw mouse data came since the open: the pointer follows it
std::atomic<bool> g_rawKeyboard { false }; // the keys come to ImGui from the raw data
std::atomic<unsigned> g_rawMouseEvents { 0 };
std::atomic<unsigned> g_rawKeyEvents { 0 };
std::atomic<unsigned> g_mouseMessages { 0 };
std::atomic<unsigned> g_keyMessages { 0 };
std::atomic<unsigned> g_otherMessages { 0 };
bool g_polledDown[5] = {}; // the drawing thread's
unsigned g_polledClicks = 0;

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

bool AsyncDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

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

// A mouse message whose position is in its window's client pixels.
bool ClientMouseMessage(UINT message)
{
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL &&
           message != WM_MOUSEHWHEEL;
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

// The modifiers as they are right now, for a click or a key that came without the window's own messages.
void TellModifiers(ImGuiIO& io)
{
    io.AddKeyEvent(ImGuiMod_Ctrl, AsyncDown(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, AsyncDown(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt, AsyncDown(VK_MENU));
    io.AddKeyEvent(ImGuiMod_Super, AsyncDown(VK_LWIN) || AsyncDown(VK_RWIN));
}

// ---------------------------------------------------------------------------------------------------------------
// The game's windows and threads, once, for the log: where the input goes in a game the menu cannot hear.

struct TopLevel
{
    char text[600];
    int at;
    unsigned count;
};

BOOL CALLBACK OneTopLevel(HWND window, LPARAM context)
{
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId())
        return TRUE;
    TopLevel& list = *reinterpret_cast<TopLevel*>(context);
    if (list.count++ < 8 && list.at >= 0 && size_t(list.at) < sizeof list.text)
    {
        char name[64] = "?";
        GetClassNameA(window, name, sizeof name);
        list.at += snprintf(list.text + list.at, sizeof list.text - size_t(list.at), "%s%p %s (thread %lu, %s)",
                            list.count > 1 ? ", " : "", window, name, thread, IsWindowVisible(window) ? "shown" : "hidden");
    }
    return TRUE;
}

void DescribeThread(DWORD thread)
{
    GUITHREADINFO info = {};
    info.cbSize = sizeof info;
    if (GetGUIThreadInfo(thread, &info))
        Log("menu: input: thread %lu: active %p, focus %p, capture %p", thread, info.hwndActive, info.hwndFocus,
            info.hwndCapture);
    else
        Log("menu: input: thread %lu: GetGUIThreadInfo failed (error %lu)", thread, GetLastError());
}

void DescribeWindows(HWND window)
{
    char name[64] = "?";
    GetClassNameA(window, name, sizeof name);
    const DWORD thread = GetWindowThreadProcessId(window, nullptr);
    const unsigned long long style = static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_STYLE));
    const unsigned long long exStyle = static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    const HWND root = GetAncestor(window, GA_ROOT);
    const HWND rootOwner = GetAncestor(window, GA_ROOTOWNER);
    Log("menu: input: the chain's window %p: class %s, %s, style 0x%llX, ex 0x%llX, thread %lu; root %p, root owner "
        "%p (thread %lu); drawn from thread %lu",
        window, name, (style & WS_CHILD) != 0 ? "a child" : "top-level", style, exStyle, thread, root, rootOwner,
        GetWindowThreadProcessId(rootOwner, nullptr), GetCurrentThreadId());
    DescribeThread(thread);
    const HWND front = GetForegroundWindow();
    DWORD frontProcess = 0;
    const DWORD frontThread = GetWindowThreadProcessId(front, &frontProcess);
    char frontName[64] = "?";
    GetClassNameA(front, frontName, sizeof frontName);
    Log("menu: input: in front: window %p, class %s, thread %lu, %s", front, frontName, frontThread,
        frontProcess == GetCurrentProcessId() ? "this process" : "another process");
    if (frontProcess == GetCurrentProcessId() && frontThread != thread)
        DescribeThread(frontThread);
    TopLevel list = {};
    EnumWindows(&OneTopLevel, reinterpret_cast<LPARAM>(&list));
    Log("menu: input: this process's top-level windows (%u): %s", list.count, list.count != 0 ? list.text : "none");
}

// ---------------------------------------------------------------------------------------------------------------
// Raw input, on the input thread.

const char* UsageName(USHORT usage) { return usage == kUsageMouse ? "mouse" : "keyboard"; }

// Keys go to ImGui from the raw data where the hooks have shown they see none of the window's input: the mouse moved
// (raw data came) and still no mouse or key message reached a hook. Where the hooks work the game's messages carry the
// keys (and their characters, as typed), and the raw data must not give them a second time.
bool KeysFromRaw()
{
    if (g_rawKeyboard.load(std::memory_order_acquire))
        return true;
    if (g_mouseMessages.load(std::memory_order_relaxed) != 0 || g_keyMessages.load(std::memory_order_relaxed) != 0 ||
        g_rawMouseEvents.load(std::memory_order_relaxed) < kRawMovesForKeys)
        return false;
    if (!g_rawKeyboard.exchange(true, std::memory_order_acq_rel))
        Log("menu: input: the mouse moved and no mouse or key message reached the hooks: the keys come from the raw "
            "data");
    return true;
}

void OnRawMouse(HWND window, const RAWMOUSE& m)
{
    g_rawMouseEvents.fetch_add(1, std::memory_order_relaxed);
    if (!InFront())
        return; // the user is elsewhere: not the menu's

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
        return; // a long frame holds the lock: a movement lost is nothing, the next one tells the position
    if (!g_capture.load(std::memory_order_acquire) || ImGui::GetCurrentContext() == nullptr)
    {
        MenuUnlock(); // closed while we waited
        return;
    }
    const LPARAM at = ScaleMouse(window, MAKELPARAM(pointer.x, pointer.y));
    if (moved || first)
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEMOVE, 0, at);
    if (m.usButtonFlags != 0)
        TellModifiers(ImGui::GetIO()); // a Ctrl+click needs Ctrl as it is now, whoever saw the key
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
}

// The characters a key press makes, from the state of the modifiers now, in the game window's keyboard layout.
int KeyCharacters(HWND window, UINT vk, UINT scan, wchar_t* out, int size)
{
    BYTE state[256] = {};
    for (int key : { VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU })
    {
        if (AsyncDown(key))
            state[key] = 0x80;
    }
    if ((GetKeyState(VK_CAPITAL) & 1) != 0)
        state[VK_CAPITAL] = 0x01;
    if (state[VK_CONTROL] != 0 && state[VK_MENU] == 0)
        return 0; // Ctrl and a key: a shortcut, no character (Ctrl+Alt is AltGr)
    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(window, nullptr));
    const int count = ToUnicodeEx(vk, scan, state, out, size, 0x4 /* leave the keyboard's state alone */, layout);
    return count > 0 ? count : 0;
}

void OnRawKey(HWND window, const RAWKEYBOARD& k)
{
    g_rawKeyEvents.fetch_add(1, std::memory_order_relaxed);
    UINT vk = k.VKey;
    if (vk == 0 || vk >= 255 || !InFront() || !KeysFromRaw())
        return; // 255: a key the keyboard makes up in its own sequences
    const bool down = (k.Flags & RI_KEY_BREAK) == 0;
    const bool e0 = (k.Flags & RI_KEY_E0) != 0;
    // The raw data says Shift, Ctrl and Alt; ImGui wants which one.
    if (vk == VK_SHIFT)
        vk = k.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL)
        vk = e0 ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU)
        vk = e0 ? VK_RMENU : VK_LMENU;
    const LPARAM lParam = (LPARAM(k.MakeCode & 0xFF) << 16) | (e0 ? LPARAM(KF_EXTENDED) << 16 : 0);
    const ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey(vk, lParam);
    wchar_t characters[4];
    const int count = down ? KeyCharacters(window, k.VKey, k.MakeCode, characters, 4) : 0;
    unsigned menuKey = 0;
    MenuKeys(&menuKey); // before the lock: it takes the lock itself

    if (!MenuTryLock(50))
        return;
    if (!g_capture.load(std::memory_order_acquire) || ImGui::GetCurrentContext() == nullptr)
    {
        MenuUnlock();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    TellModifiers(io);
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, down);
    for (int i = 0; i < count; ++i)
        io.AddInputCharacterUTF16(characters[i]);
    const bool typing = io.WantTextInput; // a number is being typed: Esc and the menu key belong to the box
    MenuUnlock();
    // Esc and the menu key close the menu, as the window's key messages do (MenuKeyPressedInWindow takes the lock).
    if (down && !typing && (k.VKey == VK_ESCAPE || (menuKey != 0 && k.VKey == menuKey)))
        MenuKeyPressedInWindow(k.VKey);
}

void OnRawInput(LPARAM lParam)
{
    alignas(8) unsigned char buffer[256];
    UINT size = sizeof buffer;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return; // larger than a mouse's or a keyboard's: some other device's
    const HWND window = g_window.load(std::memory_order_acquire);
    if (window == nullptr || !g_capture.load(std::memory_order_acquire))
        return;
    const RAWINPUT& raw = *reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw.header.dwType == RIM_TYPEMOUSE)
        OnRawMouse(window, raw.data.mouse);
    else if (raw.header.dwType == RIM_TYPEKEYBOARD)
        OnRawKey(window, raw.data.keyboard);
}

LRESULT CALLBACK InputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INPUT)
        OnRawInput(lParam);
    return DefWindowProcW(window, message, wParam, lParam); // for WM_INPUT, the system's clean-up of the raw data
}

DWORD WINAPI InputThread(void*)
{
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = &InputProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"BananaZeroInput";
    HWND window = nullptr;
    if (RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                                 nullptr);
    const DWORD error = window == nullptr ? GetLastError() : 0;
    g_inputWindow.store(window, std::memory_order_release);
    SetEvent(g_inputReady);
    if (window == nullptr)
        return error;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Our input window, the thread and the window made the first time. Null if they cannot be.
HWND InputWindow()
{
    if (g_inputThread == nullptr)
    {
        if (g_inputReady == nullptr)
            g_inputReady = CreateEventW(nullptr, TRUE, FALSE, nullptr); // kept: a late thread still sets it
        g_inputThread = g_inputReady != nullptr ? CreateThread(nullptr, 0, &InputThread, nullptr, 0, nullptr) : nullptr;
        if (g_inputThread == nullptr)
        {
            Log("menu: input: no thread of ours for the raw input (error %lu)", GetLastError());
            return nullptr;
        }
        SetThreadDescription(g_inputThread, L"Banana-Zero menu input");
        if (WaitForSingleObject(g_inputReady, 2000) != WAIT_OBJECT_0 ||
            g_inputWindow.load(std::memory_order_acquire) == nullptr)
            Log("menu: input: no window of ours for the raw input");
    }
    return g_inputWindow.load(std::memory_order_acquire);
}

RAWINPUTDEVICE Ours(USHORT usage, HWND target)
{
    RAWINPUTDEVICE device = {};
    device.usUsagePage = kUsagePageGeneric;
    device.usUsage = usage;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = target;
    return device;
}

// The mouse's and the keyboard's raw data to our window, the game's registration kept to put back.
void RegisterRaw()
{
    if (g_registered)
        return;
    const HWND target = InputWindow();
    if (target == nullptr)
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
    const RAWINPUTDEVICE ours[2] = { Ours(kUsageMouse, target), Ours(kUsageKeyboard, target) };
    g_registered = RegisterRawInputDevices(ours, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    if (g_registered)
        Log("menu: input: raw mouse and keyboard registered to our window %p; the game had %s", target, had);
    else
        Log("menu: input: RegisterRawInputDevices failed (error %lu); the game had %s", GetLastError(), had);
}

// A game may register its raw input again while the menu is open (a game switching its mouse mode), which takes the
// raw data from us: ours goes back, and the game's newest is what goes back at the close. On the drawing thread.
void KeepRaw()
{
    const HWND target = g_inputWindow.load(std::memory_order_acquire);
    if (!g_registered || target == nullptr || ++g_frames % kRegistrationEvery != 0)
        return;
    RAWINPUTDEVICE devices[64];
    UINT count = 64;
    const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1))
        return;
    for (const USHORT usage : { kUsageMouse, kUsageKeyboard })
    {
        const RAWINPUTDEVICE* now = nullptr;
        for (UINT i = 0; i < got; ++i)
            if (devices[i].usUsagePage == kUsagePageGeneric && devices[i].usUsage == usage)
                now = &devices[i];
        if (now != nullptr && now->hwndTarget == target)
            continue;
        // The game's newest, or none if it removed the registration.
        unsigned keep = 0;
        for (unsigned i = 0; i < g_savedCount; ++i)
            if (g_saved[i].usUsage != usage)
                g_saved[keep++] = g_saved[i];
        g_savedCount = keep;
        if (now != nullptr && g_savedCount < 2)
            g_saved[g_savedCount++] = *now;
        const RAWINPUTDEVICE ours = Ours(usage, target);
        const bool back = RegisterRawInputDevices(&ours, 1, sizeof ours) != FALSE;
        if (g_retaken++ == 0)
            Log("menu: input: the game registered its raw %s input again while the menu was open (%s); ours is %s",
                UsageName(usage), now != nullptr ? "to its window" : "removed", back ? "back" : "lost");
    }
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

// ---------------------------------------------------------------------------------------------------------------
// The message hook, on the game's threads.

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

// One message one of the game's threads took from its queue while the menu is open: ImGui sees it, and the game gets
// it only when the menu does not keep it (the releases, anything not mouse or keyboard). Mouse positions are told in
// the chain's window's pixels, whichever of the thread's windows the message was for.
void OnMessage(MSG* msg, HWND window)
{
    const UINT message = msg->message;
    if (message == WM_INPUT)
        return; // not the mouse's or the keyboard's while ours go to our window: the game's
    const bool mouse = MouseMessage(message);
    const bool keyboard = KeyboardMessage(message);
    if (!mouse && !keyboard)
    {
        g_otherMessages.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool release = ReleaseMessage(message);
    if (mouse)
        g_mouseMessages.fetch_add(1, std::memory_order_relaxed);
    if (keyboard)
        g_keyMessages.fetch_add(1, std::memory_order_relaxed);
    if ((mouse && g_rawMouse.load(std::memory_order_acquire)) ||
        (keyboard && g_rawKeyboard.load(std::memory_order_acquire)))
    {
        if (!release)
            Swallow(msg); // the raw data said it already
        return;
    }

    LPARAM lParam = msg->lParam;
    if (msg->hwnd != window && ClientMouseMessage(message))
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(msg->hwnd, &p);
        ScreenToClient(window, &p);
        lParam = MAKELPARAM(p.x, p.y);
    }
    // ImGui first, as in the example; then the message stays here, but for a release.
    if (!MenuTryLock(message == WM_MOUSEMOVE ? 5 : 50))
    {
        if (!release)
            SwallowKey(msg);
        return;
    }
    if (!g_capture.load(std::memory_order_acquire) || ImGui::GetCurrentContext() == nullptr)
    {
        MenuUnlock(); // closed while we waited: the game's
        return;
    }
    const LPARAM scaled = mouse && message != WM_MOUSELEAVE ? ScaleMouse(window, lParam) : lParam;
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
            DefWindowProcW(msg->hwnd, message, msg->wParam, msg->lParam);
            Swallow(msg);
            return;
        }
        SwallowKey(msg);
        return;
    }
    if (!release)
        Swallow(msg);
}

// WH_GETMESSAGE on a game thread: every message it takes from its queue (GetMessage, or PeekMessage with PM_REMOVE)
// passes here first, before the game's loop translates and dispatches it.
LRESULT CALLBACK GetMessageHook(int code, WPARAM removal, LPARAM lParam)
{
    MSG* msg = reinterpret_cast<MSG*>(lParam);
    if (code == HC_ACTION && removal == PM_REMOVE && msg != nullptr && msg->hwnd != nullptr &&
        g_capture.load(std::memory_order_acquire))
    {
        const HWND window = g_window.load(std::memory_order_acquire);
        if (window != nullptr)
            OnMessage(msg, window);
    }
    return CallNextHookEx(nullptr, code, removal, lParam);
}

void Hook(DWORD thread)
{
    if (thread == 0 || g_hookCount == kMaxHooks)
        return;
    for (unsigned i = 0; i < g_hookCount; ++i)
        if (g_hookThreads[i] == thread)
            return;
    const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, thread);
    if (hook == nullptr)
    {
        Log("menu: input: SetWindowsHookEx failed on thread %lu (error %lu)", thread, GetLastError());
        return;
    }
    g_hooks[g_hookCount] = hook;
    g_hookThreads[g_hookCount] = thread;
    ++g_hookCount;
}

// This process's thread for a window, or 0.
DWORD OurThread(HWND window)
{
    DWORD process = 0;
    const DWORD thread = window != nullptr ? GetWindowThreadProcessId(window, &process) : 0;
    return process == GetCurrentProcessId() ? thread : 0;
}

// The pointer starts where the cursor is, if that is in the window, else in the middle.
void StartCapture(HWND window)
{
    g_rawMouse.store(false, std::memory_order_release);
    g_rawKeyboard.store(false, std::memory_order_release);
    g_rawMouseEvents.store(0, std::memory_order_relaxed);
    g_rawKeyEvents.store(0, std::memory_order_relaxed);
    g_mouseMessages.store(0, std::memory_order_relaxed);
    g_keyMessages.store(0, std::memory_order_relaxed);
    g_otherMessages.store(0, std::memory_order_relaxed);
    g_frames = 0;
    g_retaken = 0;
    g_polledClicks = 0;
    for (bool& down : g_polledDown)
        down = false;
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
    if (!g_described)
    {
        g_described = true;
        DescribeWindows(window);
    }
    RegisterRaw();
}

// When neither the raw data nor a window message has brought the mouse since the open, the cursor and the buttons are
// read each frame instead, so that the menu can still be clicked. Under the menu's lock, on the drawing thread.
void Poll(HWND window)
{
    if (g_rawMouseEvents.load(std::memory_order_relaxed) != 0 || g_mouseMessages.load(std::memory_order_relaxed) != 0 ||
        !InFront() || ImGui::GetCurrentContext() == nullptr)
        return;
    ImGuiIO& io = ImGui::GetIO();
    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor))
    {
        StorePointer(cursor);
        const LPARAM at = ScaleMouse(window, MAKELPARAM(cursor.x, cursor.y));
        io.AddMousePosEvent(float(GET_X_LPARAM(at)), float(GET_Y_LPARAM(at)));
    }
    // GetAsyncKeyState sees the buttons as they are, not as Windows swaps them for a left-handed mouse.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    static const int kButtons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; ++i)
    {
        const bool down = AsyncDown(swapped && i < 2 ? kButtons[1 - i] : kButtons[i]);
        if (down == g_polledDown[i])
            continue;
        g_polledDown[i] = down;
        if (down)
            TellModifiers(io);
        io.AddMouseButtonEvent(i, down);
        if (down && g_polledClicks++ == 0)
            Log("menu: input: a click came only by reading the buttons: no raw data and no message brought it");
    }
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
    {
        KeepRaw();
        Poll(window);
        return;
    }
    StartCapture(window);
    g_capture.store(true, std::memory_order_release); // before the hooks: their first message is already the menu's
    // The window's thread, its root owner's and the one in front, where they are this process's: the window the
    // game takes its input on need not be the one it presents to.
    Hook(OurThread(window));
    Hook(OurThread(GetAncestor(window, GA_ROOTOWNER)));
    Hook(OurThread(GetForegroundWindow()));
    if (g_hookCount != 0)
    {
        g_state = "message hook on the window's thread";
        char threads[64] = "";
        int at = 0;
        for (unsigned i = 0; i < g_hookCount && at >= 0 && size_t(at) < sizeof threads; ++i)
            at += snprintf(threads + at, sizeof threads - size_t(at), "%s%lu", i != 0 ? ", " : "", g_hookThreads[i]);
        Log("menu: input: message hook on the game's thread%s %s (window %p)", g_hookCount > 1 ? "s" : "", threads,
            window);
    }
    else
        g_state = "SetWindowsHookEx failed";
    Poll(window);
}

void MenuInputRelease()
{
    if (!g_capture.exchange(false, std::memory_order_acq_rel))
        return;
    for (unsigned i = 0; i < g_hookCount; ++i)
        UnhookWindowsHookEx(g_hooks[i]); // a message a game thread is handling right now still finishes
    g_hookCount = 0;
    g_state = "released";
    RestoreRaw();
    Log("menu: input: released: %u raw mouse events, %u raw key events; through the hooks %u mouse, %u key and %u "
        "other messages; %u clicks read from the buttons%s",
        g_rawMouseEvents.load(std::memory_order_relaxed), g_rawKeyEvents.load(std::memory_order_relaxed),
        g_mouseMessages.load(std::memory_order_relaxed), g_keyMessages.load(std::memory_order_relaxed),
        g_otherMessages.load(std::memory_order_relaxed), g_polledClicks,
        g_rawKeyboard.load(std::memory_order_relaxed) ? "; the keys came from the raw data" : "");
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

void MenuInputCounts(MenuInputStats* out)
{
    out->rawMouseEvents = g_rawMouseEvents.load(std::memory_order_relaxed);
    out->rawKeyEvents = g_rawKeyEvents.load(std::memory_order_relaxed);
    out->mouseMessages = g_mouseMessages.load(std::memory_order_relaxed);
    out->keyMessages = g_keyMessages.load(std::memory_order_relaxed);
    out->otherMessages = g_otherMessages.load(std::memory_order_relaxed);
    out->hooks = g_hookCount;
    out->inputWindow = g_inputWindow.load(std::memory_order_acquire);
    out->registered = g_registered;
}
