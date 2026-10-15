#pragma once

// The menu's keyboard and mouse (M3): the way Dear ImGui's own Win32 example does it, on the game's window, but from
// a message hook (WH_GETMESSAGE) on the game's threads rather than a window procedure: each message the game's loop
// takes from the queue goes to ImGui first while the menu is open, the keys through ImGui_ImplWin32_WndProcHandler and
// the mouse as ImGui's own events (the backend's mouse handling takes and releases the game's mouse capture). The
// game's raw input registration for the mouse and the keyboard is replaced by ours, to a window of ours on a thread of
// ours, for as long as the menu is open (and put back when it closes), each device once what of it was held at the
// open is let go (ten seconds at most): the raw mouse data moves a pointer of our own, which ImGui draws, so that no
// cursor lock of the game's holds the menu's mouse still, and it reaches the menu whatever the game's loop does. Once
// the mouse's raw data is ours Windows makes no mouse messages for the game at all (RIDEV_NOLEGACY), and the game gets
// none of the key messages that pass the hooks, but the releases.

#include <windows.h>

// Starts capturing on `window` (the swap chain's): the hooks go on. Under the menu's lock, on the thread that draws,
// once per frame. A second call for the same window only keeps capturing; for another window it moves.
void MenuInputAttach(HWND window);

// In place of the backend's NewFrame (ImGui_ImplWin32_NewFrame, passed in), under the menu's lock: runs it and takes
// back the system cursor's position it tells ImGui at each frame while it sees no mouse message of its own, and tells
// our pointer instead.
void MenuInputNewFrame(void (*backendNewFrame)());

// Stops capturing: the hooks go off and the game has its raw input back.
void MenuInputRelease();

// MenuInputRelease, and the window is forgotten.
void MenuInputDetach();

// For the log and the panel: "message hook on the window's thread", "released", "removed", ...
const char* MenuInputState();

// What came since the menu last opened, for the tests.
struct MenuInputStats
{
    unsigned rawMouseEvents;
    unsigned rawKeyEvents;
    unsigned mouseMessages; // through the hooks
    unsigned keyMessages;
    unsigned otherMessages;
    unsigned probesSeen; // our own message, posted to the window at the open, seen by a hook
    unsigned retaken;    // times the game's raw registration was found over ours, and ours put back
    unsigned hooks;      // threads hooked
    HWND inputWindow; // ours, that the raw data goes to
    bool registered;  // the raw data of one device or both is registered to it
};
void MenuInputCounts(MenuInputStats* out);
