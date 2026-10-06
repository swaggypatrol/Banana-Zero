#pragma once

// The menu's keyboard and mouse (M3): the way Dear ImGui's own Win32 example does it, on the game's
// window, but from a message hook (WH_GETMESSAGE) on the window's thread rather than a window procedure: each message
// the game's loop takes from the queue goes to ImGui_ImplWin32_WndProcHandler first while the menu is open. The
// game's raw input registration for the mouse and the keyboard is replaced by ours on the same window for as long as
// the menu is open (and put back when it closes): the raw mouse data moves a pointer of our own, which ImGui draws,
// so that no cursor lock of the game's holds the menu's mouse still. While the menu is open the game gets none of
// the window's mouse and keyboard messages but the releases.

#include <windows.h>

// Starts capturing on `window` (the swap chain's): the hook goes on. Under the menu's lock, on the thread that draws,
// once per frame. A second call for the same window only keeps capturing; for another window it moves.
void MenuInputAttach(HWND window);

// Stops capturing: the hook goes off and the game has its raw input back.
void MenuInputRelease();

// MenuInputRelease, and the window is forgotten.
void MenuInputDetach();

// For the log and the panel: "message hook on the window's thread", "released", "removed", ...
const char* MenuInputState();
