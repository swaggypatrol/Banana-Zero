#pragma once

// The capture build (a branch of its own, for the Witcher 3 colour drift; never released): the finished picture, the
// back buffer as the game presents it, copied by the overlay (overlay_dx12.cpp) for the NR pass's captures
// (nr_dx12.cpp). The copy is taken before the menu, if it is open, is drawn onto the buffer.

#include <d3d12.h>

#include <cstdint>

struct CapturePicture
{
    const uint8_t* data; // `height` rows of `rowPitch` bytes, valid until CaptureDone
    unsigned width;
    unsigned height;
    unsigned rowPitch;
    DXGI_FORMAT format; // the back buffer's
    bool displayHdr;    // the display was in HDR mode
};

// Arms a copy of the back buffer at the `presents`-th present of the game's swap chain from now, patching the chain's
// Present for it when the menu has not. On the game's render thread, outside the NR pass's lock: the first call
// probes the swap chain's table (overlay_dx12.cpp). An armed or copied picture that was never taken is dropped. False,
// logged, when there is no overlay to be had or the background thread is still reading the last picture.
bool CaptureArm(ID3D12GraphicsCommandList* list, unsigned presents);

// The copy, once the GPU has made it: true with `out` filled, and the data stays until CaptureDone.
bool CaptureTake(CapturePicture* out);
void CaptureDone();

// Drops an armed or copied picture nobody will take.
void CaptureCancel();
