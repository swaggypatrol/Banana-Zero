#pragma once

// The frozen frame (M3). While the menu holds a frame frozen, every SR/RR evaluation gets
// the same input: the DLSS output the game made on the frame that was frozen, with its depth, motion vectors and
// exposure, copied into textures of ours on that frame (the guides whole, as a depth-stencil texture must be copied).
// Each frame since, the frozen picture is copied back into the game's Output before the pass runs (or instead of it,
// while NR is off), the guides are swapped for the frozen copies with the frozen frame's subrects, the motion vector
// scale is 0 and the first frame says Reset. So a slider released while frozen shows its
// effect on one and the same picture, and A/B and the split screen compare that picture with itself.
//
// The menu asks; the render thread does the work inside the NR pass, on the game's command list.

#include <d3d12.h>

struct Frame;

// The menu's request (any thread). Turning it off releases the copies a few frames later.
void FreezeRequest(bool on);
bool FreezeWanted();

// True while a frozen copy is live (the menu shows it; an Output of another size or format drops the copies and the
// request with them).
bool FreezeActive();

// From the NR pass, once per SR/RR evaluation whose frame could be read, before anything else is recorded for it:
// captures on the first frozen frame, then substitutes the frozen copies into *frame and copies the frozen picture
// into the game's Output. Returns true when *frame refers to the frozen copies. Under the pass's own lock.
bool FreezeApply(ID3D12GraphicsCommandList* list, Frame* frame);

// Retires every copy now: the game is shutting the NGX core down. Under the pass's own lock.
void FreezeRelease();

// The NR pass's (nr_dx12.cpp): takes over `count` resources a recorded command list may still read and releases them
// once the GPU is done with it. Under the pass's own lock.
void NrRetire(ID3D12Resource* const* resources, unsigned count);
