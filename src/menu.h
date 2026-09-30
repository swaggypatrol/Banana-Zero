#pragma once

// The in-game menu (M3). What the rest of the DLL calls: the hotkeys are polled once
// per SR/RR evaluation, the frame is drawn when the game presents (overlay_dx12.cpp), the panel is menu_panel.cpp,
// the state is menu_model.h.

#include <d3d12.h>

// Once per SR/RR evaluation, before the NR pass: polls the hotkeys (menu, A/B, freeze), follows a file reloaded
// behind the menu, and hands the file to the writer when a write is due. `list` is the game's command list, which
// the overlay learns the device from.
void MenuOnEvaluate(ID3D12GraphicsCommandList* list);

// Opens or closes the menu: the menu key, the overlay's Esc, the freeze key (which opens it). Any thread, not
// under the lock.
void MenuSetOpen(bool open);
bool MenuIsOpen();

// The menu's one lock: the model, the overlay and ImGui are touched under it. The overlay's frame takes it on the
// Present thread, the hotkeys on the render thread, the window messages on the window thread. Never recursive.
void MenuLock();
void MenuUnlock();
bool MenuTryLock(unsigned milliseconds); // false when the lock could not be had in that time

// A key went down in the game's window while the menu was open (menu_input.cpp): Esc and the menu key close the
// menu and are kept from the game (true). Not under the lock.
bool MenuKeyPressedInWindow(unsigned vk);
void MenuKeys(unsigned* menuKey); // the menu key as bound now

// The panel (menu_panel.cpp): between ImGui::NewFrame and ImGui::Render, under the lock.
void MenuDraw();

// What the panel works on, under the lock: the model (menu_model.h); a commit publishes the draft and logs what
// changed; the close is the panel's own button (the file is written if due, the frame unfrozen).
struct MenuModel;
MenuModel& MenuModelLocked();
void MenuCommitLocked(const char* why);
void MenuCloseLocked();

// True while the panel waits for a key to bind: the hotkeys do nothing then.
bool MenuPanelListening();

// Where the panel last drew the control for an ini key ("Intensity"), in ImGui's coordinates; false when it did
// not draw it. For menutest, which drives the panel with the mouse.
bool MenuItemRect(const char* key, float* x0, float* y0, float* x1, float* y1);

// The game is shutting the NGX core down: the menu closes, the overlay goes, the file is written now if it is due.
void MenuBeforeCoreShutdown();
