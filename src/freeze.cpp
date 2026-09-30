// The frozen frame: see freeze.h.

#include "freeze.h"

#include <atomic>
#include <cstdio>

#include "log.h"
#include "nr_frame.h"

namespace
{
constexpr unsigned kRetireFrames = 8; // command lists in flight may still read a retired copy (nr_dx12.cpp's kRing)

struct Copy
{
    ID3D12Resource* resource = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct Retired
{
    ID3D12Resource* resources[4] = {};
    unsigned long long releaseAt = 0;
};

std::atomic<bool> g_wanted { false };
std::atomic<bool> g_active { false };

// The rest belongs to the render thread, inside the NR pass's lock.
struct Freeze
{
    ID3D12Device* device = nullptr;
    Copy output; // rests in COPY_SOURCE
    Copy depth;  // rests in NON_PIXEL_SHADER_RESOURCE, like the game's own guides
    Copy motion;
    Copy exposure; // 1x1, or none
    bool havePreExposure = false;
    float preExposure = 0.0f;
    bool first = false;            // the next substituted frame is the first: Reset
    bool resetLive = false;        // the next live frame is the first after unfreezing: Reset
    unsigned long long frames = 0; // calls to FreezeApply
    unsigned long long frozenAt = 0;
    Retired retired[4];
};
Freeze g_freeze;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

// A texture like `like` (its format and the flags that matter to a copy of it), of one subrect's size.
bool Make(Copy* copy, const D3D12_RESOURCE_DESC& like, unsigned width, unsigned height, const char* what)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = like;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // A depth-stencil format needs its flag; nothing else of the game's flags is wanted on a copy.
    desc.Flags = like.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = g_freeze.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
        Log("NR: could not create the frozen %s copy (format %d, %ux%u): 0x%08lX", what, int(desc.Format), width,
            height, static_cast<unsigned long>(hr));
        return false;
    }
    *copy = { resource, width, height, desc.Format };
    return true;
}

// Copies the `width` x `height` rectangle at (x, y) of `from` (subresource 0) into `to` at (0, 0).
void CopyRect(ID3D12GraphicsCommandList* list, ID3D12Resource* from, unsigned x, unsigned y, ID3D12Resource* to,
              unsigned width, unsigned height)
{
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = from;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = source;
    destination.pResource = to;
    const D3D12_BOX box = { x, y, 0, x + width, y + height, 1 };
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
}

// The other way: the whole of `from` into `to` at (x, y).
void CopyBack(ID3D12GraphicsCommandList* list, ID3D12Resource* from, ID3D12Resource* to, unsigned x, unsigned y)
{
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = from;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = source;
    destination.pResource = to;
    list->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
}

void ReleaseRetired(bool all)
{
    for (Retired& r : g_freeze.retired)
    {
        if (r.releaseAt == 0 || (!all && r.releaseAt > g_freeze.frames))
            continue;
        for (ID3D12Resource* resource : r.resources)
        {
            if (resource != nullptr)
                resource->Release();
        }
        r = {};
    }
}

// Retires the live copies: released kRetireFrames frames on, when no recorded command list can still read them.
void Retire()
{
    Retired* slot = &g_freeze.retired[0];
    for (Retired& r : g_freeze.retired)
    {
        if (r.releaseAt == 0)
        {
            slot = &r;
            break;
        }
        if (r.releaseAt < slot->releaseAt)
            slot = &r;
    }
    if (slot->releaseAt != 0)
        ReleaseRetired(true); // every slot in use: the oldest have long since been read
    slot->resources[0] = g_freeze.output.resource;
    slot->resources[1] = g_freeze.depth.resource;
    slot->resources[2] = g_freeze.motion.resource;
    slot->resources[3] = g_freeze.exposure.resource;
    slot->releaseAt = g_freeze.frames + kRetireFrames;
    g_freeze.output = {};
    g_freeze.depth = {};
    g_freeze.motion = {};
    g_freeze.exposure = {};
    g_active.store(false, std::memory_order_release);
}

bool Capture(ID3D12GraphicsCommandList* list, const Frame& f)
{
    if (g_freeze.device == nullptr && FAILED(list->GetDevice(IID_PPV_ARGS(&g_freeze.device))))
    {
        Log("NR: freeze: the command list has no device");
        return false;
    }
    const bool exposure = f.exposure != nullptr && f.exposureView != DXGI_FORMAT_UNKNOWN;
    if (!Make(&g_freeze.output, f.outputDesc, f.width, f.height, "picture") ||
        !Make(&g_freeze.depth, f.depthDesc, f.guideWidth, f.guideHeight, "depth") ||
        !Make(&g_freeze.motion, f.motionDesc, f.motionWidth, f.motionHeight, "motion vector") ||
        (exposure && !Make(&g_freeze.exposure, f.exposureDesc, 1, 1, "exposure")))
    {
        ID3D12Resource* const made[] = { g_freeze.output.resource, g_freeze.depth.resource, g_freeze.motion.resource,
                                         g_freeze.exposure.resource };
        for (ID3D12Resource* r : made)
        {
            if (r != nullptr)
                r->Release();
        }
        g_freeze.output = {};
        g_freeze.depth = {};
        g_freeze.motion = {};
        g_freeze.exposure = {};
        return false;
    }

    // The game's textures are in the states DLSS documents on entry (Output UAV, the guides NPSR) and go back to
    // them; each copy of ours ends in the state it rests in.
    Transition(list, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyRect(list, f.output, f.baseX, f.baseY, g_freeze.output.resource, f.width, f.height);
    Transition(list, f.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, g_freeze.output.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

    Transition(list, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyRect(list, f.depth, f.depthBaseX, f.depthBaseY, g_freeze.depth.resource, f.guideWidth, f.guideHeight);
    Transition(list, f.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, g_freeze.depth.resource, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Transition(list, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyRect(list, f.motion, f.motionBaseX, f.motionBaseY, g_freeze.motion.resource, f.motionWidth, f.motionHeight);
    Transition(list, f.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, g_freeze.motion.resource, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (exposure)
    {
        Transition(list, f.exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyRect(list, f.exposure, 0, 0, g_freeze.exposure.resource, 1, 1);
        Transition(list, f.exposure, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, g_freeze.exposure.resource, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    g_freeze.havePreExposure = f.havePreExposure;
    g_freeze.preExposure = f.preExposure;
    g_freeze.first = true;
    g_freeze.frozenAt = g_freeze.frames;
    g_active.store(true, std::memory_order_release);
    char preExposure[32];
    if (f.havePreExposure)
        snprintf(preExposure, sizeof preExposure, "%.4g", double(f.preExposure));
    else
        snprintf(preExposure, sizeof preExposure, "absent");
    Log("NR: frame frozen: picture %ux%u (format %d), depth %ux%u, motion vectors %ux%u, exposure texture %s, "
        "pre-exposure %s",
        f.width, f.height, int(f.outputDesc.Format), f.guideWidth, f.guideHeight, f.motionWidth, f.motionHeight,
        exposure ? "copied" : "none", preExposure);
    return true;
}

// The frozen copies still fit this frame: same sizes and formats.
bool Fits(const Frame& f)
{
    return g_freeze.output.width == f.width && g_freeze.output.height == f.height &&
           g_freeze.output.format == f.outputDesc.Format && g_freeze.depth.width == f.guideWidth &&
           g_freeze.depth.height == f.guideHeight && g_freeze.depth.format == f.depthDesc.Format &&
           g_freeze.motion.width == f.motionWidth && g_freeze.motion.height == f.motionHeight &&
           g_freeze.motion.format == f.motionDesc.Format &&
           (g_freeze.exposure.resource != nullptr) ==
               (f.exposure != nullptr && f.exposureView != DXGI_FORMAT_UNKNOWN) &&
           (g_freeze.exposure.resource == nullptr || g_freeze.exposure.format == f.exposureDesc.Format);
}
} // namespace

void FreezeRequest(bool on) { g_wanted.store(on, std::memory_order_release); }
bool FreezeWanted() { return g_wanted.load(std::memory_order_acquire); }
bool FreezeActive() { return g_active.load(std::memory_order_acquire); }

bool FreezeApply(ID3D12GraphicsCommandList* list, Frame* f)
{
    ++g_freeze.frames;
    ReleaseRetired(false);
    const bool wanted = g_wanted.load(std::memory_order_acquire);
    const bool active = g_freeze.output.resource != nullptr;
    if (!wanted)
    {
        if (active)
        {
            Log("NR: frame unfrozen after %llu frames",
                static_cast<unsigned long long>(g_freeze.frames - g_freeze.frozenAt));
            Retire();
            g_freeze.resetLive = true;
        }
        if (g_freeze.resetLive)
        {
            f->reset = 1; // the model's history holds the frozen picture
            g_freeze.resetLive = false;
        }
        return false;
    }
    if (active && !Fits(*f))
    {
        Log("NR: the frame changed size or format while frozen; unfrozen");
        Retire();
        g_wanted.store(false, std::memory_order_release);
        f->reset = 1;
        return false;
    }
    if (!active)
    {
        if (!Capture(list, *f))
        {
            g_wanted.store(false, std::memory_order_release);
            return false;
        }
    }
    else
    {
        // The frozen picture back into the game's Output, where the pass (or nothing, with NR off) then works on it.
        Transition(list, f->output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyBack(list, g_freeze.output.resource, f->output, f->baseX, f->baseY);
        Transition(list, f->output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    f->depth = g_freeze.depth.resource;
    f->depthBaseX = 0;
    f->depthBaseY = 0;
    f->motion = g_freeze.motion.resource;
    f->motionBaseX = 0;
    f->motionBaseY = 0;
    f->exposure = g_freeze.exposure.resource;
    if (f->exposure == nullptr)
        f->exposureView = DXGI_FORMAT_UNKNOWN;
    f->havePreExposure = g_freeze.havePreExposure;
    f->preExposure = g_freeze.preExposure;
    // The picture does not move: a motion vector scale of 0 tells the model so.
    f->mvScaleX = 0.0f;
    f->mvScaleY = 0.0f;
    if (g_freeze.first)
    {
        f->reset = 1;
        g_freeze.first = false;
    }
    return true;
}

void FreezeRelease()
{
    if (g_freeze.output.resource != nullptr)
        Retire();
    ReleaseRetired(true);
    g_active.store(false, std::memory_order_release);
    g_freeze.resetLive = false;
    if (g_freeze.device != nullptr)
    {
        g_freeze.device->Release();
        g_freeze.device = nullptr;
    }
}
