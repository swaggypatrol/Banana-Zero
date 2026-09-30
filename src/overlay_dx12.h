#pragma once

// The overlay (M3): Dear ImGui drawn onto the game's back buffer when the game presents. The
// menu's glue (menu.cpp) opens and closes it; the panel (menu_panel.cpp) draws inside it; overlay_dx12.cpp says how
// the frame is reached. Every function here runs under the menu's one lock (MenuLock, menu.h), which the frame
// takes for itself on the Present thread: ImGui is touched under it and nowhere else.

#include <d3d12.h>

#include "imgui.h"

// Opens the overlay: learns the swap chain's virtual function table once (from `list`'s device), then points its
// Present and Present1 at us. False, with the reason logged, when the overlay cannot be had; `list` may be null
// once the device is known.
bool OverlayOpen(ID3D12GraphicsCommandList* list);

// Puts the table back (unless another hook came after ours, which then keeps passing through us). The ImGui context
// and the GPU objects stay for the next open.
void OverlayClose();

// Everything goes: the game is shutting the NGX core down.
void OverlayRelease();

// The preview picture (NrPreview) as a texture the panel can draw: a view in the overlay's heap, made again when
// `generation` changes. Under the lock, from the panel. Returns 0 when there is none.
ImTextureID OverlayPreviewTexture(ID3D12Resource* picture, unsigned generation, DXGI_FORMAT format);

// The frame's size (the back buffer's), for the mouse's coordinates; false while no chain is adopted.
bool OverlayFrameSize(unsigned* width, unsigned* height);

// A few words on the overlay for the log and the panel's status line: "drawing", or why it is not.
const char* OverlayState();
