#pragma once

// The menu's keyboard and mouse (M3): the way Dear ImGui's own Win32 example does it, on the game's
// window. A window subclass (SetWindowSubclass) hands each message to ImGui_ImplWin32_WndProcHandler while the
// menu is open. The game's raw input registration for the mouse and the keyboard is replaced by ours on the same
// window for as long as the menu is open (and put back when it closes): the raw mouse data moves a pointer of our
// own, which ImGui draws, so that no cursor lock of the game's holds the menu's mouse still. While the menu is
// open the game gets none of the window's mouse and keyboard messages but the releases.

#include <windows.h>

// Puts the subclass on `window` (the swap chain's) and starts capturing. Under the menu's lock, on the thread that
// draws. A second call for the same window only keeps capturing; for another window it moves.
void MenuInputAttach(HWND window);

// Stops capturing and gives the game its raw input back; the subclass stays for the next open.
void MenuInputRelease();

// MenuInputRelease, then removes the subclass (on the window's own thread, which may be a moment later).
void MenuInputDetach();

// For the log and the panel: "subclass on the window", "removed", ...
const char* MenuInputState();
