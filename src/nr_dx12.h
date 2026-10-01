#pragma once

// The Neural Rendering pass: after the game's own DLSS evaluation
// succeeded, on the same command list, run NVIDIA's NR model over the DLSS output and write the result back into it.
// Everything is lazy: the first call that carries the three textures loads the bridge and the model, initialises
// the model and creates the feature; any failure turns NR off for this process and the game runs on as before.

#include <windows.h>

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

struct NVSDK_NGX_Parameter;

// What ngx_hook.cpp knows about the DLSS feature whose evaluation just succeeded.
struct NrSource
{
    unsigned feature;     // 1 SR or 13 RR
    unsigned id;          // the handle's Id, for the log
    unsigned outWidth;    // its creation parameters; 0 when the game did not say
    unsigned outHeight;
    unsigned renderWidth;
    unsigned renderHeight;
    int createFlags;      // NVSDK_NGX_DLSS_Feature_Flags
};

// The evaluate path. `params` is the game's block: read, never written. Returns without doing anything when NR is
// off, disabled, busy, or the parameters lack a texture; whatever happens, the caller returns the core's result.
void NrAfterEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params, const NrSource& source);

// A DLSS handle was released: if NR followed it, the next SR/RR handle takes over.
void NrSourceReleased(unsigned id);

// The game is shutting the core down (Shutdown or Shutdown1), before the core does: releases the model's features and
// our GPU objects and calls the model's own Shutdown1, while the core, NvAPI and the device are all still there. Left
// to its DLL_PROCESS_DETACH, the model's clean-up faults inside NvAPI. NR stays off afterwards.
void NrBeforeCoreShutdown();

// The core is unloading. Our parameter blocks and the model's feature go with it; NR stays off afterwards.
void NrCoreGone();

// What the menu shows about the pass (M3): copied out of the render thread's state after each evaluation.
struct NrStatusState
{
    bool attempted;        // an SR/RR evaluation with textures has reached the pass
    bool off;              // NR is off for the rest of the process: `offReason` says why
    const char* offReason;
    bool ready;            // the bridge and the model are initialised
    bool haveFeature;      // the model's feature exists
    unsigned creates;      // CreateFeature calls so far (at most 64)
    uint64_t frames;       // frames the model delivered
    uint64_t failed;
    uint64_t evaluatesSeen;
    uint64_t sinceCreate;  // evaluations since the last creation (a new frame size waits until 30)
    unsigned width, height; // the frame
    unsigned feature;       // 1 SR or 13 RR
    int format;             // the Output's DXGI_FORMAT
    bool hdr;               // the DLSS feature's IsHDR flag
    bool linear;            // the encode treated the last frame as linear HDR
    bool havePreExposure;
    bool exposureTexture;
    double lastEvaluateAt;  // LogClock() of the last evaluation that reached the pass
};

// Copies the latest state out; false (and zeroes) while nothing has reached the pass yet. Any thread; waits at most
// for the render thread's copy in.
bool NrStatus(NrStatusState* out);

// "NR 1200 frames, 3 skipped, 0 failed" or the reason it is off, for the reporter's line and the totals at exit.
// Returns 0, writing nothing, while NR has never been attempted (so a game without DLSS textures logs nothing new).
size_t NrDescribe(char* out, size_t size);

// The preview for the menu to show: what the model sees (Preview = input: the proxy, with
// the zebra stripes when Zebra is on) or what it made (Preview = output: its output, before the composite), reduced to
// about a fifth of a 4K frame's width, and the statistics the menu shows beside it.
constexpr unsigned kNrPreviewBins = 192;     // the histogram: largest channel / white point, in 1/8 EV bins from -16 EV
constexpr int kNrPreviewEvMin = -16;
constexpr unsigned kNrPreviewBinsPerEv = 8;

struct NrPreviewState
{
    // The picture: R16G16B16A16_FLOAT, display-encoded (sRGB) values from 0 to 1 with alpha 1, to be shown as they are
    // on an SDR target. It rests in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE between frames: draw it in that state,
    // on the queue the game's DLSS command lists run on, and leave it in that state. nullptr while there is none.
    ID3D12Resource* picture;
    unsigned width;
    unsigned height;
    unsigned scale;        // frame pixels per picture pixel along each axis
    unsigned generation;   // changes whenever `picture` does (a new texture, or none): make the view of it again
    uint64_t pictureFrame; // the NR frame that last rendered it

    // The statistics of every pixel of a frame a few frames back (a readback ring; nothing waits for the GPU).
    bool haveStats;        // false until the first has arrived since the preview was last dropped
    uint64_t statsFrame;
    bool linearHdr;        // false: the frame was display-encoded already, so the white point, the shoulder, the stripes
                           // and the histogram mean nothing (the menu greys them out)
    float white;           // the white point W, in the frame's own units
    float shoulder;        // Shoulder, as a fraction of W
    uint32_t histogram[kNrPreviewBins]; // pixels by largest channel / W
    uint32_t pixels;       // pixels looked at
    uint32_t black;        // largest channel <= 0: in no bin
    uint32_t below;        // under the first bin
    uint32_t above;        // over the last bin
    uint32_t inShoulder;   // largest channel / W above Shoulder: compressed by the encode
    uint32_t heavy;        // compressed by more than 3 EV (the curve's slope below 1/8)
    uint32_t dark;         // the proxy's largest channel below 0.5/255: 0 in 8 bits
};

// Called by the menu on every frame it shows the preview, which is only while the menu is open: copies out
// the latest state and returns true when there is a picture. The NR pass renders the picture and gathers the
// statistics as long as these calls keep coming and Preview is not off, and drops the picture half a second after
// they stop. Frames NR does not run, or the model does not deliver, render nothing, so the last picture stays. A
// picture that stops being returned (a new frame size, the preview dropped) is kept alive a few frames longer for
// command lists still in flight; the one exception is the game shutting NGX down, which frees it at once, so never
// draw a picture the latest call did not return. Waits at most for the render thread to copy the state in; any thread.
bool NrPreview(NrPreviewState* out);
