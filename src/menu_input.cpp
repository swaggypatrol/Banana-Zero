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
// told to ImGui as its mouse events; ImGui draws that pointer. The game's own registration, if any, is put back when
// the menu closes. The raw data used to go to the game's window, through the hook; in Gears of War E-Day the hook on
// that window's thread saw no message at all, raw or not, in five openings, and the menu took no click. On a thread
// of ours it comes whatever the game's loop does. Until raw data comes the window's mouse messages serve, as in the
// example, and when neither comes the cursor and the buttons are read each frame.
//
// The mouse never goes through the Win32 backend's mouse messages, from any thread: for those the backend takes and
// releases the mouse capture (SetCapture at a press, ReleaseCapture when the last button goes up) and asks Windows for
// WM_MOUSELEAVE (TrackMouseEvent) on the window it is given, the game's. On the game's thread that capture is the
// game's own: GTA V holds its window's, and v0.1.3, which ran the backend there, released it at every click in the
// menu, after which the game took no mouse. From a thread of ours the capture calls do nothing, but TrackMouseEvent
// still reaches the game's window. The backend's NewFrame tells ImGui where the system cursor is while it has seen no
// mouse message; that position is taken out of ImGui's queue again and our pointer told instead (MenuInputNewFrame).
// Told after it, ours made two positions in every frame, and ImGui's input trickling held a wheel notch back a frame
// for each, so the wheel, and the clicks behind it, fell further and further behind.
//
// The mouse's raw data is registered with RIDEV_NOLEGACY: once it is ours Windows makes no mouse messages for the
// game's windows at all, so a click in the menu is not also the game's. In Gears of War E-Day, whose messages never
// passed the hook, every click in the menu fired the gun, and the game registered its own raw input again within a
// second or two of each open; ours is checked every frame and put back, so that the game's lasts a frame at most.
//
// A device's raw data becomes ours only once what of it was held at the open is let go (ten seconds at most): what
// the game saw go down must come up for the game too, and with ours registered the release would come to us. GTA V,
// which takes its keys from the raw data, would keep walking, or keep a weapon wheel open on Tab and turn the mouse
// with it. The key that opened the menu is one of them: the keyboard becomes ours when it is let go. Until a device
// is ours its raw data, presses included, is the game's; a key still held after the ten seconds stays pressed for such
// a game until it is pressed again. What is held is read once, at the open, and only those keys after it: reading
// every key over and over is what a key logger does, and security software stopped a test that did it for a few
// seconds. At the close the game's registration is put back and read back; if ours is still there (the game's could
// not be put back, its window gone), ours is removed, since the mouse's RIDEV_NOLEGACY left behind would leave the
// game no mouse at all.
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
#include "imgui_internal.h" // the input queue, for MenuInputNewFrame
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
constexpr unsigned kMaxHooks = 3;        // the window's thread, its root owner's, the one in front
constexpr unsigned kRawMovesForKeys = 8; // raw mouse events with no message before the keys come from raw data
constexpr double kHeldWait = 10.0;       // seconds a device's raw data waits for what was held at the open
constexpr int kPolledButtons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };

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

// Raw input, per device: ours in the game's place while capturing, once the device has nothing held, and what the
// game had registered, to put back. Under the menu's lock: taken and kept on the drawing thread, put back wherever the
// menu closes (Esc and the menu key close it from the hooks and from our input thread too).
struct RawDevice
{
    USHORT usage;
    bool taken;  // ours is registered
    bool given;  // not to be tried again before the next open: ours failed, or the game's could not be read
    bool waited; // something was held when the menu opened (logged once)
    bool saved;  // the game had a registration of its own: `game`
    RAWINPUTDEVICE game;
    BYTE held[8]; // the keys or buttons held at the open
    unsigned heldCount;
};
RawDevice g_raw[2] = { { kUsageMouse }, { kUsageKeyboard } };
double g_openedAt = 0.0;
unsigned g_retaken = 0; // times the game registered over ours since the open

// A message of our own, posted to the window at each open: whether the hook sees it says whether the game's thread
// takes its messages where a hook can see them at all.
UINT g_probeMessage = 0;
std::atomic<unsigned> g_probesSeen { 0 };

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

// The mouse as ImGui's own events, not the backend's messages (see the top). Under the menu's lock.
void TellPointer(ImGuiIO& io, HWND window, POINT p) // in the window's client pixels
{
    const LPARAM at = ScaleMouse(window, MAKELPARAM(p.x, p.y));
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(float(GET_X_LPARAM(at)), float(GET_Y_LPARAM(at)));
}

void TellButton(ImGuiIO& io, int button, bool down)
{
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMouseButtonEvent(button, down);
}

// In notches, as the backend tells WM_MOUSEWHEEL (up is positive) and WM_MOUSEHWHEEL (right is negative).
void TellWheel(ImGuiIO& io, float x, float y)
{
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMouseWheelEvent(x, y);
}

// A mouse message one of the game's threads took, for ImGui. Its position is in the window's client pixels, but for
// WM_NCMOUSEMOVE's, which is on the screen as Windows gives it. Messages ImGui has no use for (WM_MOUSELEAVE, ...) say
// nothing.
void TellMouseMessage(ImGuiIO& io, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    switch (message)
    {
    case WM_NCMOUSEMOVE:
        if (!ScreenToClient(window, &p))
            return;
        [[fallthrough]];
    case WM_MOUSEMOVE:
        StorePointer(p); // told again after the backend's NewFrame
        TellPointer(io, window, p);
        return;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        TellButton(io, 0, message != WM_LBUTTONUP);
        return;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP:
        TellButton(io, 1, message != WM_RBUTTONUP);
        return;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONUP:
        TellButton(io, 2, message != WM_MBUTTONUP);
        return;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_XBUTTONUP:
        TellButton(io, GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4, message != WM_XBUTTONUP);
        return;
    case WM_MOUSEWHEEL:
        TellWheel(io, 0.0f, float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA));
        return;
    case WM_MOUSEHWHEEL:
        TellWheel(io, -float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA), 0.0f);
        return;
    default:
        return;
    }
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
    ImGuiIO& io = ImGui::GetIO();
    if (moved || first)
        TellPointer(io, window, pointer);
    if (m.usButtonFlags != 0)
        TellModifiers(io); // a Ctrl+click needs Ctrl as it is now, whoever saw the key
    struct Button
    {
        USHORT down, up;
        int button;
    };
    static const Button kButtons[] = {
        { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0 },
        { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1 },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2 },
        { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3 },
        { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4 },
    };
    for (const Button& b : kButtons)
    {
        if ((m.usButtonFlags & b.down) != 0)
            TellButton(io, b.button, true);
        if ((m.usButtonFlags & b.up) != 0)
            TellButton(io, b.button, false);
    }
    if ((m.usButtonFlags & RI_MOUSE_WHEEL) != 0)
        TellWheel(io, 0.0f, float(SHORT(m.usButtonData)) / float(WHEEL_DELTA));
    if ((m.usButtonFlags & RI_MOUSE_HWHEEL) != 0)
        TellWheel(io, -float(SHORT(m.usButtonData)) / float(WHEEL_DELTA), 0.0f);
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

// Ours: to our window, from the background too; the mouse without the messages Windows would make from it.
RAWINPUTDEVICE Ours(USHORT usage, HWND target)
{
    RAWINPUTDEVICE device = {};
    device.usUsagePage = kUsagePageGeneric;
    device.usUsage = usage;
    device.dwFlags = RIDEV_INPUTSINK | (usage == kUsageMouse ? RIDEV_NOLEGACY : 0);
    device.hwndTarget = target;
    return device;
}

// This process's raw registration for `usage` now: 1 and `out`, 0 for none, -1 if it cannot be read.
int Registration(USHORT usage, RAWINPUTDEVICE* out)
{
    RAWINPUTDEVICE devices[64];
    UINT count = 64;
    const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1))
        return -1;
    for (UINT i = 0; i < got; ++i)
        if (devices[i].usUsagePage == kUsagePageGeneric && devices[i].usUsage == usage)
        {
            *out = devices[i];
            return 1;
        }
    return 0;
}

// Keys whose state says nothing of a key the game saw go down: VK_PACKET (characters typed by SendInput), the IME's
// own (Kana or Hangul, Kanji or Hanja, the process key), and the gamepad's, which some drivers keep reading as held.
bool Ignored(int vk)
{
    return vk == VK_PACKET || vk == VK_PROCESSKEY || vk == VK_KANA || vk == VK_KANJI || (vk >= 0xC3 && vk <= 0xDA);
}

// What of the device is held at the open: the mouse's five buttons, or any other key. Read once (see the top).
void ReadHeld(RawDevice& d)
{
    d.heldCount = 0;
    for (int vk = 1; vk < 0xFF && d.heldCount < sizeof d.held; ++vk)
    {
        const bool button =
            vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
        if (button == (d.usage == kUsageMouse) && !Ignored(vk) && AsyncDown(vk))
            d.held[d.heldCount++] = BYTE(vk);
    }
}

// The first key or button held at the open that is still held now, or 0.
int StillHeld(const RawDevice& d)
{
    for (unsigned i = 0; i < d.heldCount; ++i)
        if (AsyncDown(d.held[i]))
            return d.held[i];
    return 0;
}

// Ours in the game's place, the game's registration kept to put back, for each device that has nothing held, or has
// waited long enough (see the top). On the drawing thread, at the open and at each frame after it until both are
// ours.
void TakeRaw()
{
    const HWND target = g_inputWindow.load(std::memory_order_acquire);
    if (target == nullptr || ((g_raw[0].taken || g_raw[0].given) && (g_raw[1].taken || g_raw[1].given)))
        return;
    const double waited = LogClock() - g_openedAt;
    char names[32] = "";
    char had[192] = "";
    int named = 0, at = 0;
    bool letGo = false; // one of them waited for a release
    for (RawDevice& d : g_raw)
    {
        if (d.taken || d.given)
            continue;
        const int held = StillHeld(d);
        if (held != 0 && waited < kHeldWait)
        {
            if (!d.waited)
                Log("menu: input: the raw %s data stays the game's until what was held at the open is let go "
                    "(key 0x%02X)",
                    UsageName(d.usage), held);
            d.waited = true;
            continue;
        }
        RAWINPUTDEVICE game = {};
        const int found = Registration(d.usage, &game);
        if (found < 0)
        {
            d.given = true; // nothing to put back by: the game's stays
            Log("menu: input: the raw registrations could not be read (error %lu): the %s data stays the game's",
                GetLastError(), UsageName(d.usage));
            continue;
        }
        const RAWINPUTDEVICE ours = Ours(d.usage, target);
        if (!RegisterRawInputDevices(&ours, 1, sizeof ours))
        {
            d.given = true;
            Log("menu: input: RegisterRawInputDevices failed for the %s (error %lu)", UsageName(d.usage),
                GetLastError());
            continue;
        }
        d.taken = true;
        d.saved = found > 0;
        d.game = game;
        letGo = letGo || (d.waited && held == 0);
        named += snprintf(names + named, sizeof names - size_t(named), "%s%s", named != 0 ? " and " : "",
                          UsageName(d.usage));
        if (d.saved)
            at += snprintf(had + at, sizeof had - size_t(at), "%s%s -> window %p, flags 0x%lX", at != 0 ? ", " : "",
                           UsageName(d.usage), game.hwndTarget, game.dwFlags);
        if (held != 0)
            Log("menu: input: key 0x%02X is still held %.1f s after the open: the raw %s data is ours all the same, "
                "and a game that reads it from the raw data keeps it pressed until it is pressed again",
                held, waited, UsageName(d.usage));
    }
    if (named != 0)
        Log("menu: input: raw %s registered to our window %p%s; the game had %s", names, target,
            letGo ? " once let go" : "", at != 0 ? had : "no registration of its own");
}

// A game may register its raw input again while the menu is open (a game switching its mouse mode), which takes the
// raw data from us and gives it the mouse's messages back: ours goes back at the next frame, and the game's newest is
// what goes back at the close. On the drawing thread, every frame.
void KeepRaw()
{
    const HWND target = g_inputWindow.load(std::memory_order_acquire);
    if ((!g_raw[0].taken && !g_raw[1].taken) || target == nullptr)
        return;
    RAWINPUTDEVICE devices[64];
    UINT count = 64;
    const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1))
        return;
    for (RawDevice& d : g_raw)
    {
        if (!d.taken)
            continue;
        const RAWINPUTDEVICE* now = nullptr;
        for (UINT i = 0; i < got; ++i)
            if (devices[i].usUsagePage == kUsagePageGeneric && devices[i].usUsage == d.usage)
                now = &devices[i];
        if (now != nullptr && now->hwndTarget == target)
            continue;
        // The game's newest, or none if it removed the registration.
        d.saved = now != nullptr;
        if (now != nullptr)
            d.game = *now;
        const RAWINPUTDEVICE ours = Ours(d.usage, target);
        const bool back = RegisterRawInputDevices(&ours, 1, sizeof ours) != FALSE;
        if (g_retaken++ == 0)
            Log("menu: input: the game registered its raw %s input again while the menu was open (%s); ours is %s",
                UsageName(d.usage), now != nullptr ? "to its window" : "removed", back ? "back" : "lost");
    }
}

// The game's registrations back, and read back: ours must not outlive the menu (see the top).
void RestoreRaw()
{
    bool any = false;
    bool ok = true;
    for (RawDevice& d : g_raw)
    {
        if (!d.taken)
            continue;
        any = true;
        d.taken = false;
        RAWINPUTDEVICE back = {};
        back.usUsagePage = kUsagePageGeneric;
        back.usUsage = d.usage;
        back.dwFlags = RIDEV_REMOVE; // the game had none: ours goes
        if (d.saved)
            back = d.game;
        if (!RegisterRawInputDevices(&back, 1, sizeof(RAWINPUTDEVICE)))
        {
            ok = false;
            Log("menu: input: the game's %s registration could not be put back (error %lu)", UsageName(d.usage),
                GetLastError());
        }
    }
    if (!any)
        return;
    const HWND ours = g_inputWindow.load(std::memory_order_acquire);
    for (const RawDevice& d : g_raw)
    {
        RAWINPUTDEVICE now = {};
        if (ours == nullptr || Registration(d.usage, &now) <= 0 || now.hwndTarget != ours)
            continue;
        RAWINPUTDEVICE remove = {};
        remove.usUsagePage = kUsagePageGeneric;
        remove.usUsage = d.usage;
        remove.dwFlags = RIDEV_REMOVE;
        ok = false;
        if (RegisterRawInputDevices(&remove, 1, sizeof remove))
            Log("menu: input: ours was still the raw %s registration after the close: removed, the game has none "
                "until it registers again",
                UsageName(d.usage));
        else
            Log("menu: input: ours is still the raw %s registration after the close and could not be removed "
                "(error %lu)",
                UsageName(d.usage), GetLastError());
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
    if (message == g_probeMessage && g_probeMessage != 0)
    {
        g_probesSeen.fetch_add(1, std::memory_order_relaxed);
        Swallow(msg);
        return;
    }
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
    // The mouse as ImGui's events: the backend's handler would take and release the game's capture (see the top).
    LRESULT handled = 0;
    if (mouse)
        TellMouseMessage(ImGui::GetIO(), window, message, msg->wParam, lParam);
    else
        handled = ImGui_ImplWin32_WndProcHandler(window, message, msg->wParam, lParam);
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

// The key for ImGui's mouse button `i` as GetAsyncKeyState sees the buttons: as they are, not as Windows swaps them for
// a left-handed mouse.
int PolledButton(int i)
{
    return GetSystemMetrics(SM_SWAPBUTTON) != 0 && i < 2 ? kPolledButtons[1 - i] : kPolledButtons[i];
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
    g_probesSeen.store(0, std::memory_order_relaxed);
    g_retaken = 0;
    g_polledClicks = 0;
    for (int i = 0; i < 5; ++i)
        g_polledDown[i] = AsyncDown(PolledButton(i)); // a button already held is no click
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
    const HWND input = InputWindow(); // at the first open our thread and its window are made, which can take a while
    g_openedAt = LogClock();
    for (RawDevice& d : g_raw)
    {
        d.given = d.waited = false; // `taken` went at the last close
        ReadHeld(d);
    }
    if (input != nullptr)
        TakeRaw();
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
        TellPointer(io, window, cursor);
    }
    for (int i = 0; i < 5; ++i)
    {
        const bool down = AsyncDown(PolledButton(i));
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

// At the open nothing ImGui was told before it is kept. A button or key the menu saw go down before the last close
// came up after it, for the game (a slider dragged as End closed the menu stayed grabbed, and followed the pointer at
// the next open until a click), and events still queued at the close would come in at the next open.
void ForgetInput()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
    g_front = -1; // the focus is told again: the queue it was told in is gone
    FollowFocus();
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
        TakeRaw();
        KeepRaw();
        Poll(window);
        return;
    }
    StartCapture(window);
    ForgetInput();
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
    // A message of ours behind whatever the window has queued: the close says whether a hook saw it. The game's window
    // procedure, should it get it instead, leaves an unknown message alone.
    if (g_probeMessage == 0)
        g_probeMessage = RegisterWindowMessageW(L"BananaZeroInputProbe");
    if (g_probeMessage != 0)
        PostMessageW(window, g_probeMessage, 0, 0);
    Poll(window);
}

void MenuInputNewFrame(void (*backendNewFrame)())
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    const HWND window = g_window.load(std::memory_order_acquire);
    if (context == nullptr || window == nullptr || !g_capture.load(std::memory_order_acquire))
    {
        backendNewFrame();
        return;
    }
    // Nothing else can queue an event meanwhile: the hooks and the input thread take the menu's lock first.
    ImVector<ImGuiInputEvent>& queue = context->InputEventsQueue;
    const int before = queue.Size;
    backendNewFrame();
    for (int i = queue.Size - 1; i >= before; --i)
        if (queue[i].Type == ImGuiInputEventType_MousePos)
            queue.erase(queue.Data + i);
    TellPointer(ImGui::GetIO(), window, LoadPointer()); // nothing queued when it is where ImGui has it already
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
        "other messages, and our own %s; the game took its raw input back %u times; %u clicks read from the "
        "buttons%s",
        g_rawMouseEvents.load(std::memory_order_relaxed), g_rawKeyEvents.load(std::memory_order_relaxed),
        g_mouseMessages.load(std::memory_order_relaxed), g_keyMessages.load(std::memory_order_relaxed),
        g_otherMessages.load(std::memory_order_relaxed),
        g_probesSeen.load(std::memory_order_relaxed) != 0 ? "message seen" : "message never seen", g_retaken,
        g_polledClicks, g_rawKeyboard.load(std::memory_order_relaxed) ? "; the keys came from the raw data" : "");
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
    out->probesSeen = g_probesSeen.load(std::memory_order_relaxed);
    out->retaken = g_retaken;
    out->hooks = g_hookCount;
    out->inputWindow = g_inputWindow.load(std::memory_order_acquire);
    out->registered = g_raw[0].taken || g_raw[1].taken;
}
