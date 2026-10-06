// The Neural Rendering pass. See nr_dx12.h: the pass itself came in M2, the HDR encode, the composite, the statistics
// and the frame dump in M4.
//
// Shape of a frame (all on the game's command list, right after its DLSS evaluation):
//   1. Output UAV -> NPSR; the scene meter        5. model output UAV -> NPSR; with ModelScale below 100%, the fit
//      (linear HDR only).                            (nr_fit.hlsl): the model's change as local functions of a guide.
//   2. encode: Output -> proxy (RGBA16F), at the  6. Output NPSR -> UAV; composite: the model's change -> Output,
//      frame's size or (ModelScale) smaller.         in place, plus the badge and the calibration card.
//   3. proxy UAV -> NPSR; typeless guides cloned. 7. every resource back to the state it arrived in.
//   4. the model: Color = proxy, Depth/MVec = the
//      game's (or a typed clone of the subrect),
//      Output = our model-output texture.
// On the frames the background thread asks for them, the statistics passes run beside 2 and 6 and the frame dump
// beside 6; both are copied into readback buffers that the CPU looks at a few frames later, once the frame numbers
// written at both ends show the copy has landed (no fence, no wait). While the menu shows the preview, the
// preview picture is rendered after 6 and the statistics run whenever the last copy has landed.
//
// While the menu is open, timestamps around the pass and around the model are resolved into a readback ring the
// same way, for its GPU time; a slot is read when it comes round again, kRing frames later.
//
// Nothing else reads back, waits or allocates per frame; the descriptors come from a ring, the constants are root
// constants. One lock guards the whole state and is only ever tried, never waited for, on the evaluate path: a
// second thread evaluating at the same time skips its frame. A second, small lock guards what the menu is handed of
// the preview; the menu holds it only to copy that out.

#include "nr_dx12.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "build_hash.h"
#include "freeze.h"
#include "list_state.h"
#include "log.h"
#include "ngx_hook.h"
#include "nr_fit_shader.h"
#include "nr_frame.h"
#include "nr_shader.h"
#include "nr_shared.h"
#include "nr_stats_shader.h"
#include "nvsdk_ngx.h"
#include "settings.h"

namespace
{
constexpr int kFeatureNR = 18;
constexpr unsigned kRing = 8;                 // frames whose descriptors are kept apart; also how long retired
                                              // handles and textures are parked before release
constexpr unsigned kSrvs = 7;                 // one descriptor table: t0-t6, then u0-u2 (nr_shared.h)
constexpr unsigned kUav0 = kSrvs;
constexpr unsigned kUav1 = kSrvs + 1;
constexpr unsigned kUav2 = kSrvs + 2;
constexpr unsigned kTable = kSrvs + 3;
constexpr unsigned kTablesPerFrame = 8;       // encode, composite, dump, preview, meter, dilate, sky, fit
constexpr unsigned kDescriptorsPerFrame = kTable * kTablesPerFrame;
constexpr uint64_t kEvaluatesBetweenCreates = 30; // the driver's feature slots
constexpr unsigned kMaxCreates = 64;
constexpr uint64_t kFailuresBeforeOff = 600;  // consecutive model failures before NR stops trying
constexpr unsigned kMaxModelLogLines = 200;
constexpr size_t kMaxParked = 8;

constexpr unsigned kStatsSlots = 4;           // the readback ring
constexpr uint64_t kStatsSlotBytes = (NR_S_WORDS * 4 + 255) / 256 * 256;
constexpr uint64_t kStatsLatency = 3;         // frames before a statistics copy is looked at
constexpr uint64_t kStatsGiveUp = 60;         // ... and after which it is written off
constexpr uint64_t kDumpLatency = 8;
constexpr uint64_t kDumpGiveUp = 240;
constexpr unsigned kMaxDumps = 16;            // per process: each is about 25 MB at 4K
constexpr unsigned kPreviewWidth = 768;       // the preview picture's width to aim for: a fifth of a 4K frame
constexpr double kPreviewHold = 0.5;          // seconds the preview is kept after the menu last asked for it
constexpr unsigned kTimestamps = 4;           // per frame: the pass begins, the model begins, the model ends, the
                                              // pass ends
constexpr unsigned kTimingSamples = 16;       // the menu is shown the median of the last this many frames
constexpr double kTimingHold = 0.5;           // seconds the pass is timed after the menu last asked
constexpr uint64_t kModelSizeSettle = 60;     // frames a new model size holds before it is logged (ModelScale)
static_assert(kNrPreviewBins == NR_HIST_BINS && kNrPreviewEvMin == NR_HIST_EV_MIN &&
                  kNrPreviewBinsPerEv == NR_HIST_PER_EV,
              "the menu is handed the statistics' own histogram (nr_shared.h)");

constexpr unsigned long long kOurAppId = 0x42414E41ull; // "BANA"
constexpr unsigned long long kOldAppId = 0x24480451ull; // an older tool's, the last resort
constexpr int kSdkVersion = NVSDK_NGX_Version_API;       // 0x15

// ---------------------------------------------------------------------------------------------------------------
// The bridge (bridge/bridge.cpp) and the core exports we need.

using BzLoad = int(__cdecl*)(const wchar_t*);
using BzInit = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
using BzPopulate = int(__cdecl*)(void*);
using BzCreate = int(__cdecl*)(void*, int, void*, void**);
using BzEvaluate = int(__cdecl*)(void*, const void*, void*, void*);
using BzRelease = int(__cdecl*)(void*);
using BzShutdown = int(__cdecl*)(void*);
using BzBuild = const char*(__cdecl*)();

struct Bridge
{
    HMODULE module = nullptr;
    BzLoad load = nullptr;
    BzInit init = nullptr;
    BzPopulate populate = nullptr;
    BzCreate create = nullptr;
    BzEvaluate evaluate = nullptr;
    BzRelease release = nullptr;
    BzShutdown shutdown = nullptr;
    BzBuild build = nullptr;
};

using AllocateParameters = decltype(&NVSDK_NGX_D3D12_AllocateParameters);
using GetCapabilityParameters = decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters);

template <typename Function> Function Proc(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// ---------------------------------------------------------------------------------------------------------------
// Why a frame was not run. Each reason is counted, and logged the first time it happens.

enum Skip
{
    kSkipNone,
    kSkipOff,         // NR is off for this process: a failure earlier, or the core went away
    kSkipDisabled,    // Enabled=0
    kSkipNoParams,    // the evaluation carried no parameter block
    kSkipNoOutput,    // ... no Output texture
    kSkipNoDepth,
    kSkipNoMotion,
    kSkipGeometry,    // sizes or formats we cannot work with
    kSkipOtherSource, // a second SR/RR handle; NR follows the first
    kSkipBusy,        // another thread is inside NR
    kSkipCreated,     // the model's feature was created this frame; its first evaluation is next frame
    kSkipWaiting,     // a rebuild is due but too soon after the last one
    kSkipTypedLoad,   // the GPU cannot read the Output's format through a UAV, which the in-place composite needs
    kSkipState,       // what the game has bound on this list is not known yet (list_state.h)
    kSkipCount
};

const char* const kSkipText[kSkipCount] = {
    "",
    "NR is off",
    "Enabled=0 in dlssnr.ini",
    "no parameters",
    "no Output in the parameters",
    "no Depth in the parameters",
    "no MotionVectors in the parameters",
    "the frame's geometry or formats",
    "another SR/RR handle; NR follows the first one it saw",
    "another thread was inside NR",
    "the model's feature was created this frame",
    "a rebuild is due but the last creation was fewer than 30 evaluations ago",
    "the GPU cannot load the Output's format through a UAV (typed UAV load), which the in-place composite needs",
    "what the game has bound on this command list is not known yet, so it could not be bound again afterwards",
};

const char* ResultName(int result)
{
    switch (unsigned(result))
    {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------------------
// Formats. The model wants typed guides; a typeless depth or motion texture is cloned (its subrect only) into a
// typed one where a typed resource format exists. The two depth-stencil families have none (their typed names are
// view-only formats), so they are handed over as they are and the model's answer goes in the log.

DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
    }
}

// The format for our own views of the game's Output. sRGB has no UAV form, so both views use the plain format and
// the bits go round unchanged.
DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM: return format;
    default: break;
    }
    const DXGI_FORMAT typed = TypedFormat(format);
    return typed != format ? typed : DXGI_FORMAT_UNKNOWN;
}

// The format we read the game's exposure texture with (its first channel as a float); UNKNOWN when it has none.
DXGI_FORMAT ExposureFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT: return format;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The format our passes read the game's depth with: its depth plane alone, as one channel, so that a depth-stencil
// texture reads as the depth it holds. UNKNOWN: a format we do not know; DilateMotion and the sky then do nothing.
DXGI_FORMAT DepthView(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The format our passes read the game's motion vectors with, and the one the dilated ones are stored in, so that they
// are the same numbers. UNKNOWN: a format we do not know; DilateMotion then does nothing.
DXGI_FORMAT MotionView(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM: return format;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS: return TypedFormat(format);
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32_TYPELESS: return "R32G32_TYPELESS";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    default: return nullptr;
    }
}

// "R32_TYPELESS" or "format 39".
void DescribeFormat(DXGI_FORMAT format, char* out, size_t size)
{
    const char* name = FormatName(format);
    if (name != nullptr)
        snprintf(out, size, "%s", name);
    else
        snprintf(out, size, "format %d", int(format));
}

// ---------------------------------------------------------------------------------------------------------------
// Reading the game's evaluation parameters. Typed first, then the RR spelling, then untyped.

ID3D12Resource* GetResource(const NVSDK_NGX_Parameter* params, const char* name, const char* alternative)
{
    ID3D12Resource* resource = nullptr;
    if (params->Get(name, &resource) == NVSDK_NGX_Result_Success && resource != nullptr)
        return resource;
    resource = nullptr;
    if (params->Get(alternative, &resource) == NVSDK_NGX_Result_Success && resource != nullptr)
        return resource;
    void* untyped = nullptr;
    if (params->Get(name, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);
    untyped = nullptr;
    if (params->Get(alternative, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);
    return nullptr;
}

unsigned GetUnsigned(const NVSDK_NGX_Parameter* params, const char* name, unsigned fallback)
{
    unsigned value = 0;
    if (params->Get(name, &value) == NVSDK_NGX_Result_Success)
        return value;
    int signedValue = 0;
    if (params->Get(name, &signedValue) == NVSDK_NGX_Result_Success && signedValue >= 0)
        return unsigned(signedValue);
    return fallback;
}

float GetFloat(const NVSDK_NGX_Parameter* params, const char* name, float fallback)
{
    float value = 0.0f;
    return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}

unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }
unsigned Groups(unsigned size, unsigned per) { return (size + per - 1) / per; }

// One evaluation as the game described it, in the terms the pipeline needs.
// struct Frame: nr_frame.h (M3 moved it out, for the frozen frame).

// Fills `frame` from the game's parameters. A reason if the frame cannot be run.
Skip ReadFrame(const NVSDK_NGX_Parameter* params, const NrSource& source, Frame* frame)
{
    frame->output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    if (frame->output == nullptr)
        return kSkipNoOutput;
    frame->depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    if (frame->depth == nullptr)
        return kSkipNoDepth;
    frame->motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    if (frame->motion == nullptr)
        return kSkipNoMotion;

    frame->outputDesc = frame->output->GetDesc();
    frame->depthDesc = frame->depth->GetDesc();
    frame->motionDesc = frame->motion->GetDesc();
    if (frame->outputDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || frame->outputDesc.SampleDesc.Count != 1)
        return kSkipGeometry;

    frame->baseX = GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    frame->baseY = GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
    const unsigned textureWidth = unsigned(frame->outputDesc.Width);
    const unsigned textureHeight = frame->outputDesc.Height;
    if (frame->baseX >= textureWidth || frame->baseY >= textureHeight)
        return kSkipGeometry;
    // The DLSS output size from the creation parameters; the texture's own size when the game did not give it.
    frame->width = source.outWidth != 0 ? Min(source.outWidth, textureWidth - frame->baseX) : textureWidth - frame->baseX;
    frame->height =
        source.outHeight != 0 ? Min(source.outHeight, textureHeight - frame->baseY) : textureHeight - frame->baseY;

    frame->depthInverted = (source.createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame->lowResMotion = (source.createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame->hdr = (source.createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    // The guides: what the game rendered wins over the texture's size, bounded by the texture.
    const unsigned depthWidth = unsigned(frame->depthDesc.Width);
    const unsigned depthHeight = frame->depthDesc.Height;
    frame->depthBaseX = Min(GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0), depthWidth);
    frame->depthBaseY = Min(GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0), depthHeight);
    unsigned renderWidth = GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0);
    unsigned renderHeight = GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0);
    frame->renderSubrectGiven = renderWidth != 0 && renderHeight != 0;
    if (!frame->renderSubrectGiven)
    {
        renderWidth = source.renderWidth != 0 ? source.renderWidth : depthWidth;
        renderHeight = source.renderHeight != 0 ? source.renderHeight : depthHeight;
    }
    frame->guideWidth = Min(renderWidth, depthWidth - frame->depthBaseX);
    frame->guideHeight = Min(renderHeight, depthHeight - frame->depthBaseY);

    const unsigned motionTextureWidth = unsigned(frame->motionDesc.Width);
    const unsigned motionTextureHeight = frame->motionDesc.Height;
    frame->motionBaseX =
        Min(GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0), motionTextureWidth);
    frame->motionBaseY =
        Min(GetUnsigned(params, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0), motionTextureHeight);
    frame->motionWidth = Min(frame->lowResMotion ? frame->guideWidth : frame->width,
                             motionTextureWidth - frame->motionBaseX);
    frame->motionHeight = Min(frame->lowResMotion ? frame->guideHeight : frame->height,
                              motionTextureHeight - frame->motionBaseY);
    if (frame->width == 0 || frame->height == 0 || frame->guideWidth == 0 || frame->guideHeight == 0 ||
        frame->motionWidth == 0 || frame->motionHeight == 0)
        return kSkipGeometry;

    frame->mvScaleX = GetFloat(params, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    frame->mvScaleY = GetFloat(params, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    frame->reset = GetUnsigned(params, NVSDK_NGX_Parameter_Reset, 0);

    // The white point's sources (WhiteSource = exposure).
    float preExposure = 0.0f;
    frame->havePreExposure = params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;
    frame->preExposure = preExposure;
    frame->exposure = GetResource(params, NVSDK_NGX_Parameter_ExposureTexture, "DLSSD.ExposureTexture");
    frame->exposureView = DXGI_FORMAT_UNKNOWN;
    if (frame->exposure != nullptr)
    {
        frame->exposureDesc = frame->exposure->GetDesc();
        if (frame->exposureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            frame->exposureDesc.SampleDesc.Count == 1)
            frame->exposureView = ExposureFormat(frame->exposureDesc.Format);
    }
    return kSkipNone;
}

// ---------------------------------------------------------------------------------------------------------------
// State.

struct Texture
{
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
};

// A buffer of ours: in the default heap (the GPU writes it through a UAV; it rests in the COMMON state between
// frames) or in the readback heap (the GPU copies into it; mapped for good, the CPU reads it).
struct Buffer
{
    ID3D12Resource* resource = nullptr;
    const uint8_t* mapped = nullptr;
    uint64_t size = 0;
};

// A feature and the resources its recorded commands may still reference, released kRing frames after retirement.
struct Parked
{
    void* handle = nullptr;
    ID3D12Resource* resources[4] = {};
    uint64_t releaseAt = 0;
};

// What the CPU knew about a frame whose statistics or dump it recorded: goes into the log line or the file header.
struct FrameInfo
{
    uint64_t frame = 0; // our frame counter; the GPU's tags are this + 1
    unsigned feature = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned modelWidth = 0; // the model's picture (ModelScale)
    unsigned modelHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned flags = 0; // NrConstants.flags
    bool havePreExposure = false;
    float preExposure = 0.0f;
    float whiteScale = 1.0f;
    bool modelRan = false;
    unsigned generation = 0;
    InputType inputType = InputType::Auto;
    WhiteSource whiteSource = WhiteSource::Exposure;
    float whiteEV = 0.0f;
    float shoulder = 0.0f;
    float detail = 0.0f;
    float colour = 0.0f;
    float maxGain = 0.0f;
    float highlight = 0.0f;
};

// The statistics of one frame, handed from the render thread to the background thread that logs them.
struct Sample
{
    FrameInfo info;
    uint32_t words[NR_S_WORDS];
};
Sample g_sample;
std::atomic<int> g_sampleState { 0 }; // 0: the render thread may fill g_sample. 1: the background thread may read it.

// A frame dump goes Idle -> Recorded (commands in the game's list) -> Ready (the copy has landed; render thread)
// -> Writing -> Idle (background thread). The readback buffer is only read while Writing.
enum DumpState
{
    kDumpIdle,
    kDumpRecorded,
    kDumpReady,
    kDumpWriting,
};

// What changes all at once for the model when it changes (Reset then, so that its history does not blend
// the old input into the new).
struct EncodeKey
{
    InputType inputType = InputType::Auto;
    WhiteSource whiteSource = WhiteSource::Exposure;
    float whiteEV = 0.0f;
    float shoulder = 0.0f;
    bool card = false;
    Corner cardCorner = Corner::BottomLeft;

    bool operator==(const EncodeKey&) const = default;
};

struct Nr
{
    // Counts, read by the reporter thread without the lock.
    std::atomic<bool> attempted { false };
    std::atomic<bool> off { false };
    std::atomic<const char*> offReason { nullptr };
    std::atomic<uint64_t> frames { 0 };
    std::atomic<uint64_t> failed { 0 };
    std::atomic<uint64_t> skipped[kSkipCount] = {};
    std::atomic<bool> said[kSkipCount] = {};
    std::atomic<bool> coreGone { false };

    // Set by the background thread, taken by the render thread.
    std::atomic<bool> statsWanted { false };
    std::atomic<bool> dumpWanted { false };
    std::atomic<int> dumpState { kDumpIdle };

    // Set by the menu (NrPreview): LogClock() of its last call.
    std::atomic<double> previewAskedAt { -1.0e9 };
    // Set by the menu (NrStatus): LogClock() of its last call. The pass is timed on the GPU while it keeps asking.
    std::atomic<double> timingAskedAt { -1.0e9 };

    // The rest is under g_lock.
    bool settingsLoaded = false;
    bool ready = false;
    wchar_t directory[MAX_PATH] = {}; // where dxgi.dll is, with the trailing backslash
    wchar_t dataPath[MAX_PATH] = {};  // what the model's Init_Ext accepted
    ID3D12Device* device = nullptr;
    Bridge bridge;
    NVSDK_NGX_Parameter* createParams = nullptr;
    NVSDK_NGX_Parameter* evalParams = nullptr;
    NVSDK_NGX_Parameter* capability = nullptr; // the core's, for the fallbacks; never destroyed
    bool ownBlocks = false;                   // false: the capability block is used for both
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipeline = nullptr;      // nr.hlsl
    ID3D12PipelineState* statsPipeline = nullptr; // nr_stats.hlsl
    ID3D12PipelineState* fitPipeline = nullptr;   // nr_fit.hlsl
    ID3D12DescriptorHeap* heap = nullptr;
    UINT descriptorSize = 0;
    const char* initAttempt = nullptr; // which Init_Ext attempt the model accepted

    // Which DLSS handle NR follows. Atomics rather than under g_lock: NrSourceReleased runs on the game's thread
    // that releases the handle, which must never wait for the render thread.
    std::atomic<bool> bound { false };
    std::atomic<unsigned> sourceId { 0 };

    void* handle = nullptr; // the model's feature
    unsigned createdWidth = 0;
    unsigned createdHeight = 0;
    unsigned creates = 0;
    uint64_t evaluatesSeen = 0; // SR/RR evaluations that reached us with their textures
    uint64_t createdAt = 0;     // evaluatesSeen at the last creation
    bool resetNext = false;     // the next evaluation says Reset
    uint64_t failRun = 0;
    bool haveEncodeKey = false;
    EncodeKey encodeKey;
    DXGI_FORMAT checkedFormat = DXGI_FORMAT_UNKNOWN; // the Output format whose typed UAV loads were checked
    bool typedLoad = false;

    Texture proxy;
    Texture modelOutput;
    Texture depthClone;
    Texture motionClone;
    Parked parked[kMaxParked];

    // ModelScale: the model's picture as last evaluated (the frame's size, or smaller; 0 before the feature's first
    // evaluation), since which frame, and the size last logged. The fit's three textures (nr_fit.hlsl), at the
    // frame's size so that the slider can move without making them again: made the first frame the model works on a
    // smaller copy, kept until the frame's size changes. They rest in the UAV state.
    unsigned modelWidth = 0;
    unsigned modelHeight = 0;
    uint64_t modelSizeFrame = 0;
    unsigned loggedModelWidth = 0;
    unsigned loggedModelHeight = 0;
    Texture slope;
    Texture value;
    Texture raw;
    bool fitFailed = false;

    // The pass's GPU time, for the menu: timestamps around the pass and around the model, resolved into a readback
    // ring. Made the first time the menu asks.
    ID3D12QueryHeap* queries = nullptr;
    Buffer timingReadback;      // kRing slots of kTimestamps ticks
    bool timed[kRing] = {};     // the slot was given a frame's timestamps that have not been looked at
    uint64_t lastTick = 0;      // the end of the last frame measured: a slot holding older ticks was not written yet
    double ticksPerMs = 0.0;
    bool timingFailed = false;
    float passSamples[kTimingSamples] = {};
    float modelSamples[kTimingSamples] = {};
    unsigned samples = 0;       // how many of them hold a measurement
    unsigned nextSample = 0;
    float passMs = 0.0f;        // their medians
    float modelMs = 0.0f;

    // What the game's depth adds (DilateMotion, the sky sliders): made the first frame each is wanted and again when
    // the guides change size or format, kept otherwise. Both rest in the UAV state.
    Texture dilated; // the motion vectors dilated by depth, in the motion vectors' own typed format
    Texture mask;    // the control mask: R16G16B16A16_FLOAT, one texel per depth texel
    bool dilatedFailed = false;
    bool maskFailed = false;
    bool saidDepth = false; // what the depth's passes read has been logged

    // Statistics: made the first time the background thread or the preview asks.
    Buffer stats;
    Buffer statsReadback; // kStatsSlots slots of kStatsSlotBytes
    bool statsFailed = false;
    bool statsPending = false;
    bool statsForLog = false; // the copy in flight was asked for by the log; else by the preview alone
    FrameInfo statsInfo;

    // The preview picture: made the first frame the menu asks for it, dropped when it stops asking.
    Texture preview;
    unsigned previewScale = 0;
    unsigned previewGeneration = 0;
    bool previewFailed = false;

    // The scene meter: made the first linear HDR frame, kept for the rest of the process.
    Texture scene;           // 1x1 R32_FLOAT: the scene white point as an exposure (1 / W); rests in UAV
    Buffer meter;            // its histogram (nr_shared.h); rests in COMMON
    bool meterFailed = false;
    bool meterFresh = true;  // the next resolve starts afresh rather than easing from the last value
    double meterClock = 0.0; // LogClock() of the last metered frame

    // The frame dump: made the first time one is due, and again when the frame's size changes.
    Buffer dump;
    Buffer dumpReadback;
    unsigned dumpWidth = 0;
    unsigned dumpHeight = 0;
    bool dumpFailed = false;
    FrameInfo dumpInfo; // written before dumpState becomes Ready, read by the background thread after

    uint64_t frame = 0; // NR frames recorded
    double badgeUntil = 0.0;
    unsigned badgeSize = 0;
    bool describedFrame = false;
    bool workerStarted = false;
};

Nr g_nr;
SRWLOCK g_lock = SRWLOCK_INIT;

// What NrPreview hands the menu. The render thread writes it under g_previewLock (after g_lock, never the other way
// round); the menu holds the lock only to copy it out, so the render thread never waits longer than that copy.
SRWLOCK g_previewLock = SRWLOCK_INIT;
NrPreviewState g_preview = {};

void Off(const char* reason)
{
    g_nr.offReason.store(reason, std::memory_order_relaxed);
    g_nr.off.store(true, std::memory_order_release);
    Log("NR off for the rest of this process: %s", reason);
}

void CountSkip(Skip skip, const NrSource& source)
{
    g_nr.skipped[skip].fetch_add(1, std::memory_order_relaxed);
    if (!g_nr.said[skip].load(std::memory_order_relaxed) && !g_nr.said[skip].exchange(true))
        Log("NR skipped for %s #%u: %s (logged once per reason)", source.feature == 13 ? "RR 13" : "SR 1", source.id,
            kSkipText[skip]);
}

// The model's own log lines, through the logging callback of the FeatureCommonInfo we hand its Init_Ext. Capped:
// a model that logs per frame would otherwise put the evaluate path into the file.
std::atomic<unsigned> g_modelLogLines { 0 };

void NVSDK_CONV OnModelLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature)
{
    const unsigned line = g_modelLogLines.fetch_add(1, std::memory_order_relaxed);
    if (line > kMaxModelLogLines)
        return;
    if (line == kMaxModelLogLines)
    {
        Log("model: (further model log lines are dropped)");
        return;
    }
    size_t length = message != nullptr ? strlen(message) : 0;
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        --length;
    Log("model[%d]: %.*s", int(level), int(length), message != nullptr ? message : "");
}

// ---------------------------------------------------------------------------------------------------------------
// Initialisation: settings, bridge, model, Init_Ext ladder, parameter blocks, D3D12 objects. Once; any failure
// turns NR off.

bool ModuleDirectory(wchar_t* out, size_t size)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&NrAfterEvaluate), &self))
        return false;
    const DWORD length = GetModuleFileNameW(self, out, DWORD(size));
    if (length == 0 || length >= size)
        return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash == nullptr)
        return false;
    slash[1] = L'\0';
    return true;
}

// Path = directory + name, if it fits.
bool Join(const wchar_t* directory, const wchar_t* name, wchar_t* out, size_t size)
{
    if (wcslen(directory) + wcslen(name) + 1 > size)
        return false;
    wcscpy_s(out, size, directory);
    wcscat_s(out, size, name);
    return true;
}

bool FileExists(const wchar_t* path)
{
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Where dlssnr.ini is: beside the game's exe, else beside this DLL. The two are one folder unless a loader runs the
// DLLs from elsewhere: REFramework runs every DLL of the game's folder from copies in a _storage_ folder, where an
// ini put in the game's folder was not seen. With the file in neither, beside the exe, where the menu then writes it.
bool IniPath(wchar_t* out, size_t size)
{
    wchar_t directory[MAX_PATH];
    wchar_t beside[MAX_PATH];
    bool haveExe = false;
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    wchar_t* slash = length != 0 && length < MAX_PATH ? wcsrchr(directory, L'\\') : nullptr;
    if (slash != nullptr)
    {
        slash[1] = L'\0';
        haveExe = Join(directory, L"dlssnr.ini", out, size);
    }
    if (haveExe && FileExists(out))
        return true;
    if (ModuleDirectory(directory, MAX_PATH) && Join(directory, L"dlssnr.ini", beside, MAX_PATH) &&
        (!haveExe || FileExists(beside)))
        return wcscpy_s(out, size, beside) == 0;
    return haveExe;
}

// %LOCALAPPDATA%\Banana-Zero\<name>, created if need be. False if it cannot be made.
bool LocalFolder(const wchar_t* name, wchar_t* out, size_t size)
{
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", out, DWORD(size));
    if (length == 0 || length >= size || wcscat_s(out, size, L"\\Banana-Zero") != 0)
        return false;
    CreateDirectoryW(out, nullptr);
    if (wcscat_s(out, size, L"\\") != 0 || wcscat_s(out, size, name) != 0)
        return false;
    CreateDirectoryW(out, nullptr);
    const DWORD attributes = GetFileAttributesW(out);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool LoadBridge()
{
    wchar_t path[MAX_PATH];
    if (!Join(g_nr.directory, L"banana.nvngx.dll", path, MAX_PATH))
        return false;
    Bridge& b = g_nr.bridge;
    b.module = LoadLibraryW(path);
    if (b.module == nullptr)
    {
        Log("NR: %ls did not load (%lu)", path, GetLastError());
        return false;
    }
    b.load = Proc<BzLoad>(b.module, "bz_load");
    b.init = Proc<BzInit>(b.module, "bz_init");
    b.populate = Proc<BzPopulate>(b.module, "bz_populate");
    b.create = Proc<BzCreate>(b.module, "bz_create");
    b.evaluate = Proc<BzEvaluate>(b.module, "bz_evaluate");
    b.release = Proc<BzRelease>(b.module, "bz_release");
    b.shutdown = Proc<BzShutdown>(b.module, "bz_shutdown");
    b.build = Proc<BzBuild>(b.module, "bz_build");
    if (b.load == nullptr || b.init == nullptr || b.populate == nullptr || b.create == nullptr ||
        b.evaluate == nullptr || b.release == nullptr || b.shutdown == nullptr || b.build == nullptr)
    {
        Log("NR: %ls lacks the bz_* exports; it is not our bridge", path);
        return false;
    }
    const char* build = b.build();
    if (build == nullptr || strcmp(build, BUILD_HASH) != 0)
    {
        Log("NR: the bridge is build %s, dxgi.dll is build %s; the two must come from one build", build ? build : "?",
            BUILD_HASH);
        return false;
    }
    Log("NR: bridge %ls, build %s", path, build);
    return true;
}

bool LoadModel()
{
    wchar_t path[MAX_PATH];
    if (!Join(g_nr.directory, L"nvngx_dlssnr.dll", path, MAX_PATH))
        return false;
    const int found = g_nr.bridge.load(path);
    if (found < 0)
    {
        Log("NR: the model %ls did not load (%lu)", path, GetLastError());
        return false;
    }
    if (found != 63)
    {
        Log("NR: the model %ls lacks entry points (mask 0x%X of 0x3F)", path, unsigned(found));
        return false;
    }
    Log("NR: model %ls, its six D3D12 entry points found", path);
    return true;
}

// The model's Init_Ext, our way first, then an older tool's way item by item. Records what worked.
bool InitModel()
{
    wchar_t ourData[MAX_PATH] = {};
    const bool haveOurData = LocalFolder(L"ngx", ourData, MAX_PATH);
    wchar_t temp[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (tempLength == 0 || tempLength >= MAX_PATH)
        temp[0] = L'\0';
    if (!haveOurData)
        Log("NR: could not make %%LOCALAPPDATA%%\\Banana-Zero\\ngx; using %ls", temp);
    const wchar_t* ours = haveOurData ? ourData : temp;

    // Zeroed as the header lays it out, plus our log callback so the model's own lines land in dlssnr.log.
    static NVSDK_NGX_FeatureCommonInfo withLog = {};
    withLog.LoggingInfo.LoggingCallback = &OnModelLog;
    withLog.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    withLog.LoggingInfo.DisableOtherLoggingSinks = true;
    static const NVSDK_NGX_FeatureCommonInfo zeroed = {};

    struct Attempt
    {
        const char* name;
        unsigned long long appId;
        const wchar_t* dataPath;
        const void* info;
    };
    const Attempt attempts[] = {
        { "own app id, own data dir, zeroed info with our log callback", kOurAppId, ours, &withLog },
        { "own app id, own data dir, zeroed info", kOurAppId, ours, &zeroed },
        { "old app id, own data dir, zeroed info", kOldAppId, ours, &zeroed },
        { "old app id, %TEMP%, zeroed info", kOldAppId, temp, &zeroed },
        { "old app id, %TEMP%, the core's capability block as the 5th argument", kOldAppId, temp, g_nr.capability },
    };
    for (const Attempt& attempt : attempts)
    {
        if (attempt.info == nullptr || attempt.dataPath[0] == L'\0')
            continue;
        const int result = g_nr.bridge.init(attempt.appId, attempt.dataPath, g_nr.device, kSdkVersion, attempt.info);
        Log("NR: Init_Ext(%s, app id 0x%llX, %ls) -> 0x%08X %s", attempt.name, attempt.appId, attempt.dataPath,
            unsigned(result), ResultName(result));
        if (result == NVSDK_NGX_Result_Success)
        {
            g_nr.initAttempt = attempt.name;
            wcscpy_s(g_nr.dataPath, MAX_PATH, attempt.dataPath);
            return true;
        }
    }
    Log("NR: the model refused every Init_Ext attempt");
    return false;
}

// Our own blocks from the core's AllocateParameters; the capability block if that is not to be had.
bool MakeBlocks(HMODULE core)
{
    const auto allocate = Proc<AllocateParameters>(core, "NVSDK_NGX_D3D12_AllocateParameters");
    NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_NotImplemented;
    if (allocate != nullptr)
    {
        result = allocate(&g_nr.createParams);
        if (result == NVSDK_NGX_Result_Success)
            result = allocate(&g_nr.evalParams);
    }
    if (result == NVSDK_NGX_Result_Success && g_nr.createParams != nullptr && g_nr.evalParams != nullptr)
    {
        g_nr.ownBlocks = true;
        Log("NR: two parameter blocks of our own from the core's AllocateParameters");
    }
    else
    {
        Log("NR: AllocateParameters -> 0x%08X %s; falling back to the core's capability block", unsigned(result),
            allocate != nullptr ? ResultName(int(result)) : "(not exported)");
        if (g_nr.capability == nullptr)
            return false;
        g_nr.createParams = g_nr.capability;
        g_nr.evalParams = g_nr.capability;
        g_nr.ownBlocks = false;
    }
    // The model publishes its own callbacks into a block; both blocks get them.
    const int populated = g_nr.bridge.populate(g_nr.createParams);
    const int populatedEval = g_nr.createParams != g_nr.evalParams ? g_nr.bridge.populate(g_nr.evalParams) : populated;
    Log("NR: PopulateParameters_Impl -> 0x%08X %s (create block), 0x%08X %s (evaluate block)", unsigned(populated),
        ResultName(populated), unsigned(populatedEval), ResultName(populatedEval));
    return true;
}

bool MakePipelineState(const void* bytecode, size_t size, ID3D12PipelineState** out, const char* what)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = g_nr.rootSignature;
    pipeline.CS.pShaderBytecode = bytecode;
    pipeline.CS.BytecodeLength = size;
    const HRESULT hr = g_nr.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(out));
    if (FAILED(hr))
    {
        Log("NR: CreateComputePipelineState(%s) failed 0x%08lX", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// One root signature for the three shaders: the constants (b0), then one table of seven SRVs and three UAVs
// (nr_shared.h).
bool MakePipeline()
{
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d3d12 != nullptr ? Proc<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(d3d12, "D3D12SerializeRootSignature")
                                            : nullptr;
    if (serialize == nullptr)
    {
        Log("NR: D3D12SerializeRootSignature is not to be had from d3d12.dll");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrvs;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kTable - kSrvs;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = NR_CONSTANTS_DWORDS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = parameters;

    // Version 1.0: every descriptor volatile, so a slot a pass does not use may hold a null descriptor, and the
    // table can name a texture that is in another state while the pass that runs does not read it.
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        Log("NR: root signature serialisation failed 0x%08lX%s%s", static_cast<unsigned long>(hr),
            error != nullptr ? ": " : "", error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
        if (error != nullptr)
            error->Release();
        return false;
    }
    hr = g_nr.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&g_nr.rootSignature));
    blob->Release();
    if (FAILED(hr))
    {
        Log("NR: CreateRootSignature failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    if (!MakePipelineState(nr_cso, sizeof nr_cso, &g_nr.pipeline, "nr.hlsl") ||
        !MakePipelineState(nr_stats_cso, sizeof nr_stats_cso, &g_nr.statsPipeline, "nr_stats.hlsl") ||
        !MakePipelineState(nr_fit_cso, sizeof nr_fit_cso, &g_nr.fitPipeline, "nr_fit.hlsl"))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kRing * kDescriptorsPerFrame;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_nr.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_nr.heap));
    if (FAILED(hr))
    {
        Log("NR: CreateDescriptorHeap failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    g_nr.descriptorSize = g_nr.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Initialise(ID3D12GraphicsCommandList* list)
{
    if (!ModuleDirectory(g_nr.directory, MAX_PATH))
    {
        Log("NR: could not find our own directory (%lu)", GetLastError());
        return false;
    }
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&g_nr.device))))
    {
        Log("NR: the command list has no device");
        return false;
    }
    const HMODULE core = NgxCoreModule();
    if (core == nullptr)
    {
        Log("NR: no patched core");
        return false;
    }
    const auto getCapability = Proc<GetCapabilityParameters>(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    if (getCapability != nullptr && getCapability(&g_nr.capability) != NVSDK_NGX_Result_Success)
        g_nr.capability = nullptr;

    if (!LoadBridge() || !LoadModel() || !InitModel() || !MakeBlocks(core) || !MakePipeline())
        return false;
    if (g_nr.coreGone.load(std::memory_order_acquire))
    {
        Log("NR: the core went away during initialisation");
        return false;
    }
    Log("NR ready: Init_Ext with %s, data dir %ls, %s", g_nr.initAttempt, g_nr.dataPath,
        g_nr.ownBlocks ? "own parameter blocks" : "the core's capability block");
    return true;
}

// Whether the GPU can load the Output's format through a UAV, as the in-place composite does. The formats DLSS
// outputs are all in D3D12's optional typed-UAV-load set or queried one by one; the answer is logged once per format.
bool TypedLoadSupported(DXGI_FORMAT view)
{
    if (view != g_nr.checkedFormat)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
        support.Format = view;
        g_nr.typedLoad =
            SUCCEEDED(g_nr.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        g_nr.checkedFormat = view;
        char name[32];
        DescribeFormat(view, name, sizeof name);
        Log("NR: typed UAV loads of %s: %s", name, g_nr.typedLoad ? "supported" : "not supported; NR stands down");
    }
    return g_nr.typedLoad;
}

// ---------------------------------------------------------------------------------------------------------------
// Textures, buffers and the model's feature.

bool MakeTexture(Texture* texture, DXGI_FORMAT format, unsigned width, unsigned height, D3D12_RESOURCE_FLAGS flags,
                 D3D12_RESOURCE_STATES state, const char* what)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr =
        g_nr.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
        char name[32];
        DescribeFormat(format, name, sizeof name);
        Log("NR: could not create the %s texture (%s %ux%u): 0x%08lX", what, name, width, height,
            static_cast<unsigned long>(hr));
        return false;
    }
    *texture = { resource, format, width, height };
    return true;
}

// A default-heap buffer (UAV, created COMMON) or a readback buffer (created COPY_DEST, mapped for good).
bool MakeBuffer(Buffer* buffer, D3D12_HEAP_TYPE type, uint64_t size, const char* what)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const bool readback = type == D3D12_HEAP_TYPE_READBACK;
    desc.Flags = readback ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* resource = nullptr;
    HRESULT hr = g_nr.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, readback ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
        Log("NR: could not create the %s buffer (%llu bytes): 0x%08lX", what, static_cast<unsigned long long>(size),
            static_cast<unsigned long>(hr));
        return false;
    }
    void* mapped = nullptr;
    if (readback)
    {
        const D3D12_RANGE all = { 0, SIZE_T(size) };
        hr = resource->Map(0, &all, &mapped);
        if (FAILED(hr))
        {
            Log("NR: could not map the %s buffer: 0x%08lX", what, static_cast<unsigned long>(hr));
            resource->Release();
            return false;
        }
    }
    *buffer = { resource, static_cast<const uint8_t*>(mapped), size };
    return true;
}

void DropBuffer(Buffer* buffer)
{
    if (buffer->resource != nullptr)
    {
        if (buffer->mapped != nullptr)
        {
            const D3D12_RANGE nothingWritten = { 0, 0 };
            buffer->resource->Unmap(0, &nothingWritten);
        }
        buffer->resource->Release();
    }
    *buffer = {};
}

bool Used(const Parked& p)
{
    if (p.handle != nullptr)
        return true;
    for (ID3D12Resource* r : p.resources)
    {
        if (r != nullptr)
            return true;
    }
    return false;
}

void Unpark(Parked* p)
{
    if (p->handle != nullptr)
    {
        const int result = g_nr.bridge.release(p->handle);
        Log("NR: released a retired feature -> 0x%08X %s", unsigned(result), ResultName(result));
    }
    for (ID3D12Resource* r : p->resources)
    {
        if (r != nullptr)
            r->Release();
    }
    *p = {};
}

// Retires a feature and the resources its recorded commands may still reference: command lists in flight may use
// them for a few more frames, so they are released kRing frames on.
void Park(void* handle, ID3D12Resource* const* resources, unsigned count)
{
    Parked* slot = nullptr;
    for (Parked& p : g_nr.parked)
    {
        if (!Used(p))
        {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr)
    {
        // Full: the oldest has waited longest; release it now.
        slot = &g_nr.parked[0];
        for (Parked& p : g_nr.parked)
        {
            if (p.releaseAt < slot->releaseAt)
                slot = &p;
        }
        Unpark(slot);
    }
    slot->handle = handle;
    for (unsigned i = 0; i < count && i < 4; ++i)
        slot->resources[i] = resources[i];
    slot->releaseAt = g_nr.frame + kRing;
}

// The fit's textures, when the frame's size changes.
void DropFit()
{
    if (g_nr.slope.resource == nullptr && g_nr.value.resource == nullptr && g_nr.raw.resource == nullptr)
        return;
    ID3D12Resource* const resources[3] = { g_nr.slope.resource, g_nr.value.resource, g_nr.raw.resource };
    Park(nullptr, resources, 3);
    g_nr.slope = {};
    g_nr.value = {};
    g_nr.raw = {};
}

// The live feature and its textures, for a new frame size.
void ParkCurrent()
{
    ID3D12Resource* const resources[4] = { g_nr.proxy.resource, g_nr.modelOutput.resource, g_nr.depthClone.resource,
                                           g_nr.motionClone.resource };
    Park(g_nr.handle, resources, 4);
    g_nr.handle = nullptr;
    g_nr.proxy = {};
    g_nr.modelOutput = {};
    g_nr.depthClone = {};
    g_nr.motionClone = {};
    g_nr.modelWidth = 0;
    g_nr.modelHeight = 0;
    DropFit();
}

void ReleaseParked()
{
    for (Parked& p : g_nr.parked)
    {
        if (Used(p) && p.releaseAt <= g_nr.frame)
            Unpark(&p);
    }
}

// One of the depth's textures (the dilated motion vectors, the control mask) at this format and size: kept while they
// stay, made again when they change (the old one parked for the command lists still in flight). One that cannot be
// made is not tried again, and what wants it does nothing.
bool EnsureGuide(Texture* texture, DXGI_FORMAT format, unsigned width, unsigned height, bool* failed, const char* what)
{
    if (texture->resource != nullptr && texture->format == format && texture->width == width &&
        texture->height == height)
        return true;
    if (*failed)
        return false;
    if (texture->resource != nullptr)
    {
        ID3D12Resource* const old[1] = { texture->resource };
        Park(nullptr, old, 1);
        *texture = {};
    }
    if (!MakeTexture(texture, format, width, height, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what))
    {
        *failed = true;
        return false;
    }
    return true;
}

// The creation keys. Of the model's own parameters it reads only Preset when it creates the feature (the rest at
// every evaluation: FillTunables); Preset is written when the user set it, and a key once written stays in the block.
void FillCreate(NVSDK_NGX_Parameter* params, unsigned width, unsigned height, const ModelSettings& m)
{
    params->Set("DLSSNR.Enabled", 1u);
    params->Set("DLSSNR.Width", width);
    params->Set("DLSSNR.Height", height);
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    if (m.preset.set)
        params->Set("DLSSNR.Hint.Render.Preset", m.preset.value);
}

// The model's own parameters, which it reads at every evaluation, so a change shows from the next frame (a change of
// Style, the two local strengths, skin structure or the auto mask also restarts the model's history). A key once
// written stays in the block, so one the user has not set is written as the model's default, which is also what the
// model takes for a key that is absent. Each with the type the model reads it as.
void FillTunables(NVSDK_NGX_Parameter* p, const ModelSettings& m)
{
    p->Set("DLSSNR.Intensity", m.intensity.set ? m.intensity.value : 1.0f);
    p->Set("DLSSNR.Style", m.style.set ? m.style.value : 0u);
    p->Set("DLSSNR.LocalStructureStrength", m.localStructure.set ? m.localStructure.value : 1.0f);
    p->Set("DLSSNR.LocalToneStrength", m.localTone.set ? m.localTone.value : 1.0f);
    p->Set("DLSSNR.SkinStructureStrength", m.skinStructure.set ? m.skinStructure.value : -1.0f);
    p->Set("DLSSNR.UseAutoMask", m.autoMask.set && m.autoMask.value != 0 ? 1 : 0);
}

void DescribeFrame(const Frame& f)
{
    char output[32], depth[32], motion[32], exposure[96], render[48], preExposure[32];
    DescribeFormat(f.outputDesc.Format, output, sizeof output);
    DescribeFormat(f.depthDesc.Format, depth, sizeof depth);
    DescribeFormat(f.motionDesc.Format, motion, sizeof motion);
    if (f.exposure != nullptr)
    {
        char format[32];
        DescribeFormat(f.exposureDesc.Format, format, sizeof format);
        snprintf(exposure, sizeof exposure, "%s %ux%u%s", format, unsigned(f.exposureDesc.Width),
                 f.exposureDesc.Height, f.exposureView == DXGI_FORMAT_UNKNOWN ? " (not a float texture: not used)" : "");
    }
    else
        snprintf(exposure, sizeof exposure, "absent");
    if (f.renderSubrectGiven)
        snprintf(render, sizeof render, "given, %ux%u", f.guideWidth, f.guideHeight);
    else
        snprintf(render, sizeof render, "not given (creation size used)");
    if (f.havePreExposure)
        snprintf(preExposure, sizeof preExposure, "%.4g", double(f.preExposure));
    else
        snprintf(preExposure, sizeof preExposure, "absent");
    Log("NR frame: Output %s %ux%u, frame %ux%u at %u,%u; Depth %s %ux%u, subrect %ux%u at %u,%u%s; MVec %s %ux%u, "
        "subrect %ux%u at %u,%u%s, scale %.4g %.4g, %s; render subrect %s; %s%s; pre-exposure %s; exposure texture %s",
        output, unsigned(f.outputDesc.Width), f.outputDesc.Height, f.width, f.height, f.baseX, f.baseY, depth,
        unsigned(f.depthDesc.Width), f.depthDesc.Height, f.guideWidth, f.guideHeight, f.depthBaseX, f.depthBaseY,
        TypedFormat(f.depthDesc.Format) != f.depthDesc.Format ? " (typeless: cloned typed)" : "", motion,
        unsigned(f.motionDesc.Width), f.motionDesc.Height, f.motionWidth, f.motionHeight, f.motionBaseX, f.motionBaseY,
        TypedFormat(f.motionDesc.Format) != f.motionDesc.Format ? " (typeless: cloned typed)" : "", double(f.mvScaleX),
        double(f.mvScaleY), f.lowResMotion ? "render resolution" : "display resolution", render, f.hdr ? "HDR" : "SDR",
        f.depthInverted ? ", depth inverted" : "", preExposure, exposure);
}

// Creates the model's feature for this frame's size, and the textures that go with it. Records into the game's
// list, so the frame that creates does not evaluate. False turns NR off.
bool CreateFeature(ID3D12GraphicsCommandList* list, const Frame& f, const ModelSettings& model)
{
    if (g_nr.creates >= kMaxCreates)
    {
        Off("the model's feature has been created 64 times; stopping to protect the driver's feature slots");
        return false;
    }
    if (!MakeTexture(&g_nr.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "proxy") ||
        !MakeTexture(&g_nr.modelOutput, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "model output"))
    {
        Off("a texture could not be created");
        return false;
    }
    const DXGI_FORMAT depthTyped = TypedFormat(f.depthDesc.Format);
    if (depthTyped != f.depthDesc.Format &&
        !MakeTexture(&g_nr.depthClone, depthTyped, f.guideWidth, f.guideHeight, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, "depth clone"))
    {
        Off("the depth clone could not be created");
        return false;
    }
    const DXGI_FORMAT motionTyped = TypedFormat(f.motionDesc.Format);
    if (motionTyped != f.motionDesc.Format &&
        !MakeTexture(&g_nr.motionClone, motionTyped, f.motionWidth, f.motionHeight, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, "motion clone"))
    {
        Off("the motion vector clone could not be created");
        return false;
    }

    ++g_nr.creates;
    FillCreate(g_nr.createParams, f.width, f.height, model);
    void* handle = nullptr;
    int result = g_nr.bridge.create(list, kFeatureNR, g_nr.createParams, &handle);
    Log("NR: CreateFeature(18, %ux%u, %s) -> 0x%08X %s, handle %p, creation %u", f.width, f.height,
        g_nr.ownBlocks ? "own block" : "capability block", unsigned(result), ResultName(result), handle, g_nr.creates);
    if (result != NVSDK_NGX_Result_Success && g_nr.ownBlocks && g_nr.capability != nullptr)
    {
        // The fallback: the model may want the block it populated the callbacks into to be the core's own.
        const int populated = g_nr.bridge.populate(g_nr.capability);
        FillCreate(g_nr.capability, f.width, f.height, model);
        handle = nullptr;
        ++g_nr.creates;
        result = g_nr.bridge.create(list, kFeatureNR, g_nr.capability, &handle);
        Log("NR: CreateFeature(18, %ux%u, capability block after PopulateParameters_Impl -> 0x%08X) -> 0x%08X %s, "
            "handle %p, creation %u",
            f.width, f.height, unsigned(populated), unsigned(result), ResultName(result), handle, g_nr.creates);
        if (result == NVSDK_NGX_Result_Success)
        {
            // Our two blocks stay allocated: DestroyParameters followed by the core's Shutdown1 (which a game may
            // call at exit) faults inside the core (nrprobe on driver 32.0.16.1714).
            g_nr.createParams = g_nr.capability;
            g_nr.evalParams = g_nr.capability;
            g_nr.ownBlocks = false;
        }
    }
    if (result != NVSDK_NGX_Result_Success || handle == nullptr)
    {
        Off("the model would not create its feature");
        return false;
    }
    g_nr.handle = handle;
    g_nr.createdWidth = f.width;
    g_nr.createdHeight = f.height;
    g_nr.createdAt = g_nr.evaluatesSeen;
    g_nr.resetNext = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The preview for the menu: the picture, and what the menu is handed.

float FloatBits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

bool PreviewWanted(const Settings& s)
{
    return s.preview != Preview::Off &&
           LogClock() - g_nr.previewAskedAt.load(std::memory_order_relaxed) < kPreviewHold;
}

// Retires the picture (command lists in flight, the menu's too, may read it for a few more frames) and tells the menu
// there is none.
void DropPreview()
{
    ID3D12Resource* const resources[1] = { g_nr.preview.resource };
    Park(nullptr, resources, 1);
    g_nr.preview = {};
    AcquireSRWLockExclusive(&g_previewLock);
    g_preview = {};
    g_preview.generation = ++g_nr.previewGeneration;
    ReleaseSRWLockExclusive(&g_previewLock);
}

// The picture for this frame size: a whole number of frame pixels per picture pixel, so that each picture pixel
// covers a square of its own (the zebra stripes mark the square if any one pixel in it qualifies), about
// kPreviewWidth wide. Made in the state it rests in between frames, the one the menu draws it in.
bool EnsurePreview(const Frame& f)
{
    const unsigned scale = f.width / kPreviewWidth > 1 ? f.width / kPreviewWidth : 1;
    const unsigned width = Groups(f.width, scale);
    const unsigned height = Groups(f.height, scale);
    if (g_nr.preview.resource != nullptr && g_nr.previewScale == scale && g_nr.preview.width == width &&
        g_nr.preview.height == height)
        return true;
    if (g_nr.previewFailed)
        return false;
    if (g_nr.preview.resource != nullptr)
        DropPreview();
    if (!MakeTexture(&g_nr.preview, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "preview"))
    {
        g_nr.previewFailed = true;
        return false;
    }
    g_nr.previewScale = scale;
    ++g_nr.previewGeneration;
    return true;
}

// After a frame rendered the picture.
void PublishPicture(uint64_t frameIndex)
{
    AcquireSRWLockExclusive(&g_previewLock);
    g_preview.picture = g_nr.preview.resource;
    g_preview.width = g_nr.preview.width;
    g_preview.height = g_nr.preview.height;
    g_preview.scale = g_nr.previewScale;
    g_preview.generation = g_nr.previewGeneration;
    g_preview.pictureFrame = frameIndex;
    ReleaseSRWLockExclusive(&g_previewLock);
}

// A statistics copy that landed while the menu shows the preview: the histogram and the counts it shows.
void PublishStats(const FrameInfo& info, const uint8_t* slot)
{
    uint32_t counts[NR_COUNTS];
    memcpy(counts, slot + NR_S_COUNTS * 4, sizeof counts);
    AcquireSRWLockExclusive(&g_previewLock);
    NrPreviewState& p = g_preview;
    memcpy(p.histogram, slot + NR_S_MAX_HIST * 4, sizeof p.histogram);
    p.haveStats = true;
    p.statsFrame = info.frame;
    p.linearHdr = (info.flags & NR_FLAG_LINEAR_HDR) != 0;
    p.white = FloatBits(counts[NR_C_WHITE]);
    p.shoulder = info.shoulder;
    p.pixels = counts[NR_C_PIXELS];
    p.black = counts[NR_C_BLACK];
    p.below = counts[NR_C_BELOW];
    p.above = counts[NR_C_ABOVE];
    p.inShoulder = counts[NR_C_SHOULDER];
    p.heavy = counts[NR_C_HEAVY];
    p.dark = counts[NR_C_DARK];
    ReleaseSRWLockExclusive(&g_previewLock);
}

// ---------------------------------------------------------------------------------------------------------------
// The statistics and the frame dump: sizes, lazy creation, collection.

bool EnsureStats()
{
    if (g_nr.stats.resource != nullptr)
        return true;
    if (g_nr.statsFailed)
        return false;
    if (MakeBuffer(&g_nr.stats, D3D12_HEAP_TYPE_DEFAULT, NR_S_WORDS * 4, "statistics") &&
        MakeBuffer(&g_nr.statsReadback, D3D12_HEAP_TYPE_READBACK, kStatsSlots * kStatsSlotBytes, "statistics readback"))
        return true;
    DropBuffer(&g_nr.stats);
    DropBuffer(&g_nr.statsReadback);
    g_nr.statsFailed = true;
    return false;
}

// The scene meter's texture and histogram. Should they fail, WhiteSource scene falls back to the game's exposure.
bool EnsureMeter()
{
    if (g_nr.scene.resource != nullptr)
        return true;
    if (g_nr.meterFailed)
        return false;
    if (MakeTexture(&g_nr.scene, DXGI_FORMAT_R32_FLOAT, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "scene white point") &&
        MakeBuffer(&g_nr.meter, D3D12_HEAP_TYPE_DEFAULT, NR_METER_WORDS * 4, "scene meter"))
    {
        g_nr.meterFresh = true;
        return true;
    }
    if (g_nr.scene.resource != nullptr)
        g_nr.scene.resource->Release();
    g_nr.scene = {};
    DropBuffer(&g_nr.meter);
    g_nr.meterFailed = true;
    Log("NR: no scene meter; WhiteSource scene uses the game's exposure instead");
    return false;
}

// Where things lie in the dump block (nr_shared.h; nr.hlsl computes the same).
struct DumpLayout
{
    unsigned reducedWidth, reducedHeight;
    unsigned cropWidth, cropHeight, cropX, cropY;
    uint64_t end;   // the closing word
    uint64_t bytes; // the whole block
};

DumpLayout Layout(unsigned width, unsigned height)
{
    DumpLayout l = {};
    l.reducedWidth = width / NR_DUMP_SCALE;
    l.reducedHeight = height / NR_DUMP_SCALE;
    l.cropWidth = Min(NR_DUMP_CROP, width);
    l.cropHeight = Min(NR_DUMP_CROP, height);
    l.cropX = (width - l.cropWidth) / 2;
    l.cropY = (height - l.cropHeight) / 2;
    l.end = NR_DUMP_HEADER_BYTES + uint64_t(NR_DUMP_PICTURES) *
                                       (uint64_t(l.reducedWidth) * l.reducedHeight + uint64_t(l.cropWidth) * l.cropHeight) *
                                       8;
    l.bytes = l.end + 8;
    return l;
}

// The dump buffers for this frame size. Only called while no dump is in flight (Idle), so the old ones, if the
// size changed, are no longer in use by the GPU or the background thread.
bool EnsureDump(const Frame& f)
{
    if (g_nr.dump.resource != nullptr && g_nr.dumpWidth == f.width && g_nr.dumpHeight == f.height)
        return true;
    if (g_nr.dumpFailed)
        return false;
    DropBuffer(&g_nr.dump);
    DropBuffer(&g_nr.dumpReadback);
    const DumpLayout layout = Layout(f.width, f.height);
    if (layout.reducedWidth == 0 || layout.reducedHeight == 0)
        return false;
    if (MakeBuffer(&g_nr.dump, D3D12_HEAP_TYPE_DEFAULT, layout.bytes, "dump") &&
        MakeBuffer(&g_nr.dumpReadback, D3D12_HEAP_TYPE_READBACK, layout.bytes, "dump readback"))
    {
        g_nr.dumpWidth = f.width;
        g_nr.dumpHeight = f.height;
        return true;
    }
    DropBuffer(&g_nr.dump);
    DropBuffer(&g_nr.dumpReadback);
    g_nr.dumpFailed = true;
    return false;
}

// A statistics copy recorded a few frames ago: once the frame number at both of its ends is right, the copy has
// landed; it goes to the background thread if the log asked for it, and to the menu while it shows the preview.
// Nothing waits: not there yet, it is looked at again next frame. Only one copy is ever in flight, so nothing writes
// the slot while it is read.
void CollectStats(bool previewing)
{
    if (!g_nr.statsPending || g_nr.frame < g_nr.statsInfo.frame + kStatsLatency)
        return;
    if (g_nr.statsForLog && g_sampleState.load(std::memory_order_acquire) != 0)
        return; // the background thread has not logged the previous one yet
    const uint8_t* slot = g_nr.statsReadback.mapped + (g_nr.statsInfo.frame % kStatsSlots) * kStatsSlotBytes;
    uint32_t first = 0, last = 0;
    memcpy(&first, slot + NR_S_TAG_FIRST * 4, 4);
    memcpy(&last, slot + NR_S_TAG_LAST * 4, 4);
    const uint32_t tag = uint32_t(g_nr.statsInfo.frame + 1);
    if (first != tag || last != tag)
    {
        if (g_nr.frame > g_nr.statsInfo.frame + kStatsGiveUp)
            g_nr.statsPending = false; // that frame's commands never ran; the next request starts afresh
        return;
    }
    g_nr.statsPending = false;
    if (previewing)
        PublishStats(g_nr.statsInfo, slot);
    if (g_nr.statsForLog)
    {
        memcpy(g_sample.words, slot, sizeof g_sample.words);
        g_sample.info = g_nr.statsInfo;
        g_sampleState.store(1, std::memory_order_release);
    }
}

void CollectDump()
{
    if (g_nr.dumpState.load(std::memory_order_relaxed) != kDumpRecorded ||
        g_nr.frame < g_nr.dumpInfo.frame + kDumpLatency)
        return;
    const DumpLayout layout = Layout(g_nr.dumpInfo.width, g_nr.dumpInfo.height);
    uint32_t first = 0, last = 0;
    memcpy(&first, g_nr.dumpReadback.mapped + NR_D_TAG * 4, 4);
    memcpy(&last, g_nr.dumpReadback.mapped + layout.end, 4);
    const uint32_t tag = uint32_t(g_nr.dumpInfo.frame + 1);
    if (first == tag && last == tag)
        g_nr.dumpState.store(kDumpReady, std::memory_order_release);
    else if (g_nr.frame > g_nr.dumpInfo.frame + kDumpGiveUp)
    {
        g_nr.dumpState.store(kDumpIdle, std::memory_order_release);
        Log("NR: the frame dump of frame %llu never arrived; dropped",
            static_cast<unsigned long long>(g_nr.dumpInfo.frame));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// ModelScale: the model on a smaller copy of the frame.

// The model's picture for this frame: the frame itself at 100%; below, that share of each side (in the slider's steps,
// ModelScaleStep), rounded to whole 8x8 tiles and at least 64 pixels. The feature stays at the frame's size and is
// handed this much of its input and output (subrects), so the slider moves without a rebuild (nrprobe's T9: the same
// picture as a feature made at that size, nothing outside the subrect touched).
void ModelSize(const Frame& f, const Settings& s, unsigned* width, unsigned* height)
{
    *width = f.width;
    *height = f.height;
    const float percent = ModelScaleStep(s.modelScale);
    if (!(percent < 100.0f))
        return;
    auto side = [percent](unsigned size) {
        const unsigned scaled = unsigned(float(size) * percent / 800.0f + 0.5f) * 8;
        return Min(scaled > 64 ? scaled : 64, size);
    };
    *width = side(f.width);
    *height = side(f.height);
}

// The fit's three textures for this frame size: made the first time the model works on a smaller copy, at the
// frame's own size, kept until that changes (ParkCurrent). Ones that cannot be made are not tried again, and the model
// then works on the whole frame.
bool EnsureFit(const Frame& f)
{
    if (g_nr.slope.resource != nullptr)
        return true;
    if (g_nr.fitFailed)
        return false;
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (MakeTexture(&g_nr.slope, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height, uav,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "fit slope") &&
        MakeTexture(&g_nr.value, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height, uav,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "fit value") &&
        MakeTexture(&g_nr.raw, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height, uav,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "fit change"))
        return true;
    DropFit();
    g_nr.fitFailed = true;
    Log("NR: ModelScale has no textures for its fit; the model works on the whole frame");
    return false;
}

// A new model size restarts the model's history, which would otherwise blend pictures of two sizes. The size is
// logged once it has held for kModelSizeSettle frames, so that a dragged slider leaves one line rather than one a
// step; a frame at 100% only once a smaller size has been logged.
void NoteModelSize(const Frame& f, unsigned width, unsigned height, uint64_t frameIndex)
{
    if (width != g_nr.modelWidth || height != g_nr.modelHeight)
    {
        if (g_nr.modelWidth != 0)
            g_nr.resetNext = true;
        g_nr.modelWidth = width;
        g_nr.modelHeight = height;
        g_nr.modelSizeFrame = frameIndex;
        return;
    }
    if (frameIndex != g_nr.modelSizeFrame + kModelSizeSettle ||
        (width == g_nr.loggedModelWidth && height == g_nr.loggedModelHeight))
        return;
    const bool whole = width == f.width && height == f.height;
    if (whole && g_nr.loggedModelWidth == 0)
        return;
    g_nr.loggedModelWidth = width;
    g_nr.loggedModelHeight = height;
    if (whole)
        Log("NR: ModelScale 100%%: the model works on the whole %ux%u frame", f.width, f.height);
    else
        Log("NR: ModelScale: the model works on %ux%u of the %ux%u frame (%.0f%% of its pixels)", width, height,
            f.width, f.height, 100.0 * double(width) * double(height) / (double(f.width) * double(f.height)));
}

// ---------------------------------------------------------------------------------------------------------------
// The pass's GPU time for the menu (NrStatus): timestamps where the pass begins, around the model and where the
// pass ends, resolved into a readback slot that is looked at when the ring comes round to it again, kRing frames
// later, long after the GPU has run the frame. Only while the menu keeps asking.

bool TimingWanted() { return LogClock() - g_nr.timingAskedAt.load(std::memory_order_relaxed) < kTimingHold; }

// The query heap and the readback ring, and the ticks' frequency: the queue's, so a queue of the list's own type,
// made only to ask. Once; should any of it fail, there is no GPU time to show.
bool EnsureTiming(ID3D12GraphicsCommandList* list)
{
    if (g_nr.queries != nullptr)
        return true;
    if (g_nr.timingFailed)
        return false;
    g_nr.timingFailed = true; // until everything below is in place
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    UINT64 frequency = 0;
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = type;
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(g_nr.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
        {
            if (FAILED(queue->GetTimestampFrequency(&frequency)))
                frequency = 0;
            queue->Release();
        }
    }
    if (frequency == 0)
    {
        Log("NR: no timestamp frequency for the game's command list (type %d); the menu shows no GPU time", int(type));
        return false;
    }
    D3D12_QUERY_HEAP_DESC heap = {};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = kRing * kTimestamps;
    const HRESULT hr = g_nr.device->CreateQueryHeap(&heap, IID_PPV_ARGS(&g_nr.queries));
    if (FAILED(hr))
    {
        g_nr.queries = nullptr;
        Log("NR: CreateQueryHeap failed 0x%08lX; the menu shows no GPU time", static_cast<unsigned long>(hr));
        return false;
    }
    if (!MakeBuffer(&g_nr.timingReadback, D3D12_HEAP_TYPE_READBACK, uint64_t(kRing) * kTimestamps * 8,
                    "timing readback"))
    {
        g_nr.queries->Release();
        g_nr.queries = nullptr;
        return false;
    }
    g_nr.ticksPerMs = double(frequency) / 1000.0;
    g_nr.timingFailed = false;
    return true;
}

float Median(const float* values, unsigned count)
{
    float sorted[kTimingSamples];
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned j = i;
        for (; j > 0 && sorted[j - 1] > values[i]; --j)
            sorted[j] = sorted[j - 1];
        sorted[j] = values[i];
    }
    return count != 0 ? sorted[count / 2] : 0.0f;
}

// The timestamps a frame kRing back left in `slot`, into the medians the menu is shown. The ticks only ever grow, so
// a slot whose first is not past the last frame measured has not been written yet (or is torn), and is passed over.
void CollectTiming(unsigned slot)
{
    if (!g_nr.timed[slot])
        return;
    g_nr.timed[slot] = false;
    uint64_t t[kTimestamps];
    memcpy(t, g_nr.timingReadback.mapped + uint64_t(slot) * kTimestamps * 8, sizeof t);
    if (t[0] <= g_nr.lastTick || t[1] < t[0] || t[2] < t[1] || t[3] < t[2])
        return;
    const double pass = double(t[3] - t[0]) / g_nr.ticksPerMs;
    if (pass > 1000.0)
        return;
    g_nr.lastTick = t[3];
    g_nr.passSamples[g_nr.nextSample] = float(pass);
    g_nr.modelSamples[g_nr.nextSample] = float(double(t[2] - t[1]) / g_nr.ticksPerMs);
    g_nr.nextSample = (g_nr.nextSample + 1) % kTimingSamples;
    if (g_nr.samples < kTimingSamples)
        ++g_nr.samples;
    g_nr.passMs = Median(g_nr.passSamples, g_nr.samples);
    g_nr.modelMs = Median(g_nr.modelSamples, g_nr.samples);
}

// The menu stopped asking: what was measured goes, so that it starts afresh next time.
void StopTiming()
{
    if (g_nr.samples == 0 && g_nr.nextSample == 0)
    {
        bool any = false;
        for (bool t : g_nr.timed)
            any |= t;
        if (!any)
            return;
    }
    for (bool& t : g_nr.timed)
        t = false;
    g_nr.samples = 0;
    g_nr.nextSample = 0;
    g_nr.passMs = 0.0f;
    g_nr.modelMs = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// The frame.

void Transition(D3D12_RESOURCE_BARRIER* barrier, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier->Transition.pResource = resource;
    barrier->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier->Transition.StateBefore = before;
    barrier->Transition.StateAfter = after;
}

void UavBarrier(D3D12_RESOURCE_BARRIER* barrier, ID3D12Resource* resource)
{
    barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier->UAV.pResource = resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_nr.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(slot) * g_nr.descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(unsigned slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = g_nr.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(slot) * g_nr.descriptorSize;
    return handle;
}

// A texture SRV at `slot`; a null one (reads as zero) when `resource` is null.
void WriteSrv(unsigned slot, ID3D12Resource* resource, DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = resource != nullptr ? format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_nr.device->CreateShaderResourceView(resource, &srv, CpuHandle(slot));
}

void WriteTextureUav(unsigned slot, ID3D12Resource* resource, DXGI_FORMAT format)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = resource != nullptr ? format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_nr.device->CreateUnorderedAccessView(resource, nullptr, &uav, CpuHandle(slot));
}

// A raw (byte address) buffer UAV; a null one when the buffer has no resource.
void WriteBufferUav(unsigned slot, const Buffer* buffer)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = buffer != nullptr ? UINT(buffer->size / 4) : 1;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_nr.device->CreateUnorderedAccessView(buffer != nullptr ? buffer->resource : nullptr, nullptr, &uav,
                                           CpuHandle(slot));
}

// One slot of a descriptor table: a texture and the format to view it with; null where the pass reads nothing.
struct View
{
    ID3D12Resource* resource;
    DXGI_FORMAT format;
};

// A table of nr.hlsl or nr_stats.hlsl (nr_shared.h): the seven SRVs, the texture in u0, the buffer in u1, nothing in
// u2.
void WriteTable(unsigned table, const View (&srvs)[kSrvs], View target, const Buffer* buffer)
{
    for (unsigned i = 0; i < kSrvs; ++i)
        WriteSrv(table + i, srvs[i].resource, srvs[i].format);
    WriteTextureUav(table + kUav0, target.resource, target.format);
    WriteBufferUav(table + kUav1, buffer);
    WriteTextureUav(table + kUav2, nullptr, DXGI_FORMAT_UNKNOWN);
}

void Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline, unsigned table,
              const NrConstants& constants, unsigned groupsX, unsigned groupsY)
{
    ID3D12DescriptorHeap* heaps[] = { g_nr.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_nr.rootSignature);
    list->SetPipelineState(pipeline);
    list->SetComputeRoot32BitConstants(0, NR_CONSTANTS_DWORDS, &constants, 0);
    list->SetComputeRootDescriptorTable(1, GpuHandle(table));
    list->Dispatch(groupsX, groupsY, 1);
}

// Both halves of a dump pass: the reduced pictures, then the crop from the centre.
void DumpPasses(ID3D12GraphicsCommandList* list, unsigned table, NrConstants constants, unsigned mode,
                const DumpLayout& layout)
{
    constants.mode = mode;
    constants.part = 0;
    constants.outWidth = layout.reducedWidth;
    constants.outHeight = layout.reducedHeight;
    Dispatch(list, g_nr.pipeline, table, constants, Groups(layout.reducedWidth, 8), Groups(layout.reducedHeight, 8));
    constants.part = 1;
    constants.outWidth = layout.cropWidth;
    constants.outHeight = layout.cropHeight;
    Dispatch(list, g_nr.pipeline, table, constants, Groups(layout.cropWidth, 8), Groups(layout.cropHeight, 8));
}

// A GPU buffer into its readback copy at `offset`; the buffer rests in COMMON again afterwards.
void CopyOut(ID3D12GraphicsCommandList* list, const Buffer& from, const Buffer& to, uint64_t offset)
{
    D3D12_RESOURCE_BARRIER barrier;
    Transition(&barrier, from.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(to.resource, offset, from.resource, 0, from.size);
    Transition(&barrier, from.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &barrier);
}

// Copies the subrect of a typeless guide into its typed clone; the source is back in NPSR when it returns.
void Clone(ID3D12GraphicsCommandList* list, ID3D12Resource* source, unsigned baseX, unsigned baseY,
           const Texture& clone)
{
    D3D12_RESOURCE_BARRIER barriers[2];
    Transition(&barriers[0], source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, barriers);
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to = from;
    to.pResource = clone.resource;
    const D3D12_BOX box = { baseX, baseY, 0, baseX + clone.width, baseY + clone.height, 1 };
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    Transition(&barriers[0], source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(&barriers[1], clone.resource, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(2, barriers);
}

// The evaluation keys. Every value every frame: a shared block would otherwise keep stale ones. The model's picture
// is `modelWidth` x `modelHeight` at the top left of the proxy and of its output (ModelScale; the frame's size at
// 100%); the depth and the motion vectors are the game's whatever its size, MVecScale included (nrprobe's T10).
void FillEvaluate(NVSDK_NGX_Parameter* p, const Frame& f, unsigned modelWidth, unsigned modelHeight,
                  ID3D12Resource* depth, unsigned depthBaseX, unsigned depthBaseY, ID3D12Resource* motion,
                  unsigned motionBaseX, unsigned motionBaseY, unsigned reset)
{
    p->Set("DLSSNR.Color", g_nr.proxy.resource);
    p->Set("DLSSNR.Depth", depth);
    p->Set("DLSSNR.MVec", motion);
    p->Set("DLSSNR.Output", g_nr.modelOutput.resource);
    p->Set("DLSSNR.Enabled", 1u);
    p->Set("DLSSNR.Width", modelWidth);
    p->Set("DLSSNR.Height", modelHeight);
    p->Set("DLSSNR.DepthInverted", f.depthInverted ? 1u : 0u);
    p->Set("DLSSNR.Reset", reset);
    p->Set("DLSSNR.ColorSubrectBaseX", 0u);
    p->Set("DLSSNR.ColorSubrectBaseY", 0u);
    p->Set("DLSSNR.ColorSubrectWidth", modelWidth);
    p->Set("DLSSNR.ColorSubrectHeight", modelHeight);
    p->Set("DLSSNR.OutputSubrectBaseX", 0u);
    p->Set("DLSSNR.OutputSubrectBaseY", 0u);
    p->Set("DLSSNR.OutputSubrectWidth", modelWidth);
    p->Set("DLSSNR.OutputSubrectHeight", modelHeight);
    p->Set("DLSSNR.DepthSubrectBaseX", depthBaseX);
    p->Set("DLSSNR.DepthSubrectBaseY", depthBaseY);
    p->Set("DLSSNR.DepthSubrectWidth", f.guideWidth);
    p->Set("DLSSNR.DepthSubrectHeight", f.guideHeight);
    p->Set("DLSSNR.MVecSubrectBaseX", motionBaseX);
    p->Set("DLSSNR.MVecSubrectBaseY", motionBaseY);
    p->Set("DLSSNR.MVecSubrectWidth", f.motionWidth);
    p->Set("DLSSNR.MVecSubrectHeight", f.motionHeight);
    p->Set("DLSSNR.MVecScaleX", f.mvScaleX);
    p->Set("DLSSNR.MVecScaleY", f.mvScaleY);
}

// The control mask (the sky sliders), or none. Every frame, like the rest. While it is given, the model leaves its own
// auto mask off (the teardown).
void FillMask(NVSDK_NGX_Parameter* p, const Texture* mask)
{
    ID3D12Resource* const resource = mask != nullptr ? mask->resource : nullptr;
    p->Set("DLSSNR.ControlMask", resource);
    p->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
    p->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
    p->Set("DLSSNR.ControlMaskSubrectWidth", mask != nullptr ? mask->width : 0u);
    p->Set("DLSSNR.ControlMaskSubrectHeight", mask != nullptr ? mask->height : 0u);
}

// Whether the sky sliders are in use: either away from 1. At 1 the mask would be 1 everywhere, which is what no mask
// does, so none is made.
bool SkyWanted(const Settings& s) { return s.skyTone != 1.0f || s.skyStructure != 1.0f; }

// Once, the first frame anything of the depth's is wanted: what its passes read, and what counts as sky.
__declspec(noinline) void DescribeDepth(const Frame& f, DXGI_FORMAT depthView, DXGI_FORMAT motionView)
{
    char depth[32], depthAs[32], motion[32], motionAs[32];
    DescribeFormat(f.depthDesc.Format, depth, sizeof depth);
    DescribeFormat(depthView, depthAs, sizeof depthAs);
    DescribeFormat(f.motionDesc.Format, motion, sizeof motion);
    DescribeFormat(motionView, motionAs, sizeof motionAs);
    Log("NR: the depth's passes read the depth (%s, %ux%u at %u,%u) %s%s, sky where it is %s; the motion vectors (%s, "
        "%ux%u at %u,%u) %s%s",
        depth, f.guideWidth, f.guideHeight, f.depthBaseX, f.depthBaseY, depthView != DXGI_FORMAT_UNKNOWN ? "as " : "",
        depthView != DXGI_FORMAT_UNKNOWN ? depthAs : "not at all: DilateMotion, the sky sliders and ShowSky do nothing",
        f.depthInverted ? "0 (inverted)" : "1", motion, f.motionWidth, f.motionHeight, f.motionBaseX, f.motionBaseY,
        motionView != DXGI_FORMAT_UNKNOWN ? "as " : "",
        motionView != DXGI_FORMAT_UNKNOWN ? motionAs : "not at all: DilateMotion does nothing");
}

bool LinearHdr(const Frame& f, const Settings& s)
{
    return s.inputType == InputType::LinearHdr || (s.inputType == InputType::Auto && f.hdr);
}

// Where this frame's white point comes from: the setting, with auto made concrete (the game's
// exposure when it gives an exposure texture, else the scene meter), and scene falling back to the game's exposure
// when there is no meter.
WhiteSource Source(const Frame& f, const Settings& s, bool meter)
{
    WhiteSource source = s.whiteSource;
    if (source == WhiteSource::Auto)
        source = f.exposureView != DXGI_FORMAT_UNKNOWN ? WhiteSource::Exposure : WhiteSource::Scene;
    if (source == WhiteSource::Scene && !meter)
        source = WhiteSource::Exposure;
    return source;
}

// The constants all passes of a frame share (the passes then set mode, part and the picture size): the encode's
// white point and shoulder, the composite's strengths, where the calibration card goes, and the model's picture.
NrConstants FrameConstants(const Frame& f, const Settings& s, WhiteSource source, uint64_t frameIndex,
                           unsigned modelWidth, unsigned modelHeight)
{
    NrConstants c = {};
    c.width = f.width;
    c.height = f.height;
    c.modelWidth = modelWidth;
    c.modelHeight = modelHeight;
    c.stepX = float(f.width) / float(modelWidth);
    c.stepY = float(f.height) / float(modelHeight);
    if (modelWidth != f.width || modelHeight != f.height)
        c.flags |= NR_FLAG_SCALED;
    c.baseX = f.baseX;
    c.baseY = f.baseY;
    c.frame = uint32_t(frameIndex + 1);
    c.shoulder = s.shoulder;
    c.detail = s.detailStrength;
    c.colour = s.colourStrength;
    c.maxGain = s.maxGainEV;
    c.highlight = s.highlightRestore;
    c.whiteScale = 1.0f;
    if (s.compare) // the split screen (M3): the original's share of the width, never the whole of it
        c.splitX = Min(unsigned(float(f.width) * s.compareSplit / 100.0f + 0.5f), f.width - 2) | 1u;
    // What the game's depth adds; Run decides which of its passes run.
    c.guideWidth = f.guideWidth;
    c.guideHeight = f.guideHeight;
    c.depthBaseX = f.depthBaseX;
    c.depthBaseY = f.depthBaseY;
    c.motionBaseX = f.motionBaseX;
    c.motionBaseY = f.motionBaseY;
    c.skyTone = s.skyTone;
    c.skyStructure = s.skyStructure;
    if (f.depthInverted)
        c.flags |= NR_FLAG_DEPTH_INVERTED;
    if (s.showSky && DepthView(f.depthDesc.Format) != DXGI_FORMAT_UNKNOWN)
        c.flags |= NR_FLAG_SHOW_SKY;
    if (!LinearHdr(f, s))
        return c; // display-encoded already: the encode is a copy, no white point, no shoulder, no card
    c.flags |= NR_FLAG_LINEAR_HDR;
    // The white point, times 2^WhiteEV: the game's pre-exposure over its exposure value, or the
    // scene meter's, which comes as an exposure texture of our own. Either texture divides on the GPU
    // (nr_common.hlsli), so its value never comes back to the CPU.
    if (source == WhiteSource::Exposure)
    {
        if (f.havePreExposure && std::isfinite(f.preExposure) && f.preExposure > 0.0f)
            c.whiteScale = f.preExposure;
        if (f.exposureView != DXGI_FORMAT_UNKNOWN)
            c.flags |= NR_FLAG_EXPOSURE;
    }
    else if (source == WhiteSource::Scene)
        c.flags |= NR_FLAG_EXPOSURE;
    c.whiteScale *= std::exp2(s.whiteEV);
    if (s.card)
    {
        // A quarter of the frame's width, 5:12, a margin of 1/24 of the height from the corner's two edges.
        const unsigned width = f.width / 4;
        const unsigned height = width * 5 / 12;
        const unsigned margin = f.height / 24;
        if (width >= 64 && height + 2 * margin < f.height)
        {
            const bool right = s.cardCorner == Corner::TopRight || s.cardCorner == Corner::BottomRight;
            const bool bottom = s.cardCorner == Corner::BottomLeft || s.cardCorner == Corner::BottomRight;
            c.cardX = right ? f.width - margin - width : margin;
            c.cardY = bottom ? f.height - margin - height : margin;
            c.cardW = width;
            c.cardH = height;
            c.flags |= NR_FLAG_CARD;
        }
    }
    return c;
}

FrameInfo Info(const Frame& f, const Settings& s, WhiteSource source, const NrConstants& c, uint64_t frameIndex,
               unsigned feature)
{
    FrameInfo info;
    info.frame = frameIndex;
    info.feature = feature;
    info.width = f.width;
    info.height = f.height;
    info.modelWidth = c.modelWidth;
    info.modelHeight = c.modelHeight;
    info.format = f.outputDesc.Format;
    info.flags = c.flags;
    info.havePreExposure = f.havePreExposure;
    info.preExposure = f.preExposure;
    info.whiteScale = c.whiteScale;
    info.generation = s.generation;
    info.inputType = s.inputType;
    info.whiteSource = source;
    info.whiteEV = s.whiteEV;
    info.shoulder = s.shoulder;
    info.detail = s.detailStrength;
    info.colour = s.colourStrength;
    info.maxGain = s.maxGainEV;
    info.highlight = s.highlightRestore;
    return info;
}

__declspec(noinline) void LogModelFailure(int result)
{
    Log("NR: the model's EvaluateFeature -> 0x%08X %s (frame %llu); the frame is left as DLSS made it", unsigned(result),
        ResultName(result), static_cast<unsigned long long>(g_nr.frames.load(std::memory_order_relaxed)));
}

__declspec(noinline) void LogModelRecovered(uint64_t failures)
{
    Log("NR: the model evaluates again after %llu failures", static_cast<unsigned long long>(failures));
}

void Run(ID3D12GraphicsCommandList* list, const Frame& f, const Settings& s, unsigned feature, bool previewing)
{
    const uint64_t frameIndex = g_nr.frame++;
    const unsigned base = unsigned(frameIndex % kRing) * kDescriptorsPerFrame;
    const unsigned encodeTable = base;
    const unsigned compositeTable = base + kTable;
    const unsigned dumpTable = base + 2 * kTable;
    const unsigned previewTable = base + 3 * kTable;
    const unsigned meterTable = base + 4 * kTable;
    const unsigned dilateTable = base + 5 * kTable;
    const unsigned skyTable = base + 6 * kTable;
    const unsigned fitTable = base + 7 * kTable;
    const DXGI_FORMAT outputView = ViewFormat(f.outputDesc.Format);

    // The GPU time while the menu asks: this frame's timestamps go where the frame kRing back left its own, which are
    // looked at first.
    const unsigned timingSlot = unsigned(frameIndex % kRing);
    const bool timing = TimingWanted() && EnsureTiming(list);
    if (timing)
    {
        CollectTiming(timingSlot);
        list->EndQuery(g_nr.queries, D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestamps);
    }
    else
        StopTiming();

    // ModelScale: the model's picture this frame, and a new history when its size changes.
    unsigned modelWidth = f.width, modelHeight = f.height;
    ModelSize(f, s, &modelWidth, &modelHeight);
    bool scaled = modelWidth != f.width || modelHeight != f.height;
    if (scaled && !EnsureFit(f))
    {
        modelWidth = f.width;
        modelHeight = f.height;
        scaled = false;
    }
    NoteModelSize(f, modelWidth, modelHeight, frameIndex);

    // The scene meter runs on every linear HDR frame, whichever white point is in use, so that one switched to it
    // finds it settled already.
    const bool meter = LinearHdr(f, s) && EnsureMeter();
    const WhiteSource source = Source(f, s, meter);
    NrConstants constants = FrameConstants(f, s, source, frameIndex, modelWidth, modelHeight);
    ID3D12Resource* exposure = nullptr;
    DXGI_FORMAT exposureView = DXGI_FORMAT_UNKNOWN;
    if (source == WhiteSource::Scene)
    {
        exposure = g_nr.scene.resource;
        exposureView = DXGI_FORMAT_R32_FLOAT;
    }
    else if ((constants.flags & NR_FLAG_EXPOSURE) != 0)
    {
        exposure = f.exposure;
        exposureView = f.exposureView;
    }

    // What the model sees changes all at once when the encode's settings do.
    const EncodeKey key = { s.inputType, source, s.whiteEV, s.shoulder, s.card, s.cardCorner };
    if (g_nr.haveEncodeKey && !(key == g_nr.encodeKey))
        g_nr.resetNext = true;
    g_nr.encodeKey = key;
    g_nr.haveEncodeKey = true;

    // What this frame carries besides the pass itself: the statistics when the log asked for them or the menu shows
    // the preview (whenever the last copy has landed), a dump (the request is only taken once the dump is recorded),
    // the preview picture while the menu asks for it.
    const bool logStats = g_nr.statsWanted.load(std::memory_order_relaxed);
    const bool stats = !g_nr.statsPending && (logStats || previewing) && EnsureStats();
    if (stats && logStats)
        g_nr.statsWanted.store(false, std::memory_order_relaxed);
    const bool dumpDue = g_nr.dumpState.load(std::memory_order_acquire) == kDumpIdle &&
                         g_nr.dumpWanted.load(std::memory_order_relaxed) && EnsureDump(f);
    const DumpLayout layout = dumpDue ? Layout(f.width, f.height) : DumpLayout {};
    const bool preview = previewing && EnsurePreview(f);

    // What the game's depth adds: the motion vectors dilated by it (DilateMotion), the control mask that sets the sky
    // apart (the sky sliders), the stripes over the sky (ShowSky, in the constants already). Each needs the depth in
    // a format our passes read, the dilation the motion vectors too.
    const DXGI_FORMAT depthView = DepthView(f.depthDesc.Format);
    const DXGI_FORMAT motionView = MotionView(f.motionDesc.Format);
    if (!g_nr.saidDepth && (s.dilateMotion || SkyWanted(s) || s.showSky))
    {
        g_nr.saidDepth = true;
        DescribeDepth(f, depthView, motionView);
    }
    const bool depthReadable = depthView != DXGI_FORMAT_UNKNOWN;
    const bool dilate = s.dilateMotion && depthReadable && motionView != DXGI_FORMAT_UNKNOWN &&
                        EnsureGuide(&g_nr.dilated, motionView, f.motionWidth, f.motionHeight, &g_nr.dilatedFailed,
                                    "dilated motion vector");
    const bool sky = SkyWanted(s) && depthReadable &&
                     EnsureGuide(&g_nr.mask, DXGI_FORMAT_R16G16B16A16_FLOAT, f.guideWidth, f.guideHeight,
                                 &g_nr.maskFailed, "control mask");
    const bool showSky = (constants.flags & NR_FLAG_SHOW_SKY) != 0;

    // The descriptor tables (nr_shared.h has which pass reads which slot).
    const View none = { nullptr, DXGI_FORMAT_UNKNOWN };
    const View frameView = { f.output, outputView };
    const View exposureV = { exposure, exposureView };
    const View proxyV = { g_nr.proxy.resource, g_nr.proxy.format };
    const View modelV = { g_nr.modelOutput.resource, g_nr.modelOutput.format };
    const View depthV = { f.depth, depthView };
    const Buffer* const statsBuffer = stats ? &g_nr.stats : nullptr;
    WriteTable(encodeTable, { frameView, none, none, exposureV, none, none, none }, proxyV, statsBuffer);
    if (scaled)
    {
        const View slopeV = { g_nr.slope.resource, g_nr.slope.format };
        const View valueV = { g_nr.value.resource, g_nr.value.format };
        const View rawV = { g_nr.raw.resource, g_nr.raw.format };
        WriteTable(compositeTable, { showSky ? depthV : none, modelV, proxyV, exposureV, slopeV, valueV, rawV },
                   frameView, statsBuffer);
        // The fit's u1 is a texture, not the buffer.
        for (unsigned i = 0; i < kSrvs; ++i)
        {
            const View v = i == 1 ? modelV : i == 2 ? proxyV : none;
            WriteSrv(fitTable + i, v.resource, v.format);
        }
        WriteTextureUav(fitTable + kUav0, g_nr.slope.resource, g_nr.slope.format);
        WriteTextureUav(fitTable + kUav1, g_nr.value.resource, g_nr.value.format);
        WriteTextureUav(fitTable + kUav2, g_nr.raw.resource, g_nr.raw.format);
    }
    else
        WriteTable(compositeTable, { showSky ? depthV : none, modelV, proxyV, exposureV, none, none, none }, frameView,
                   statsBuffer);
    if (dumpDue)
        WriteTable(dumpTable, { frameView, modelV, proxyV, exposureV, none, none, none }, none, &g_nr.dump);
    if (preview)
        WriteTable(previewTable, { none, modelV, proxyV, none, none, none, none },
                   { g_nr.preview.resource, g_nr.preview.format }, nullptr);
    if (meter)
        WriteTable(meterTable, { frameView, none, none, none, none, none, none },
                   { g_nr.scene.resource, DXGI_FORMAT_R32_FLOAT }, &g_nr.meter);
    if (dilate)
        WriteTable(dilateTable, { depthV, { f.motion, motionView }, none, none, none, none, none },
                   { g_nr.dilated.resource, g_nr.dilated.format }, nullptr);
    if (sky)
        WriteTable(skyTable, { depthV, none, none, none, none, none, none }, { g_nr.mask.resource, g_nr.mask.format },
                   nullptr);
    const unsigned groupsX = Groups(f.width, 8);
    const unsigned groupsY = Groups(f.height, 8);
    const unsigned modelX = Groups(modelWidth, 8); // the encode at the model's size, and the fit (8x8 texels a group)
    const unsigned modelY = Groups(modelHeight, 8);
    const unsigned statsX = Groups(f.width, 32); // nr_stats.hlsl: 16x16 threads, 2x2 pixels each
    const unsigned statsY = Groups(f.height, 32);

    // 1-2: the scene meter, the encode, and the frame's statistics.
    D3D12_RESOURCE_BARRIER barriers[12];
    unsigned count = 0;
    Transition(&barriers[count++], f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (stats)
        Transition(&barriers[count++], g_nr.stats.resource, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (meter)
        Transition(&barriers[count++], g_nr.meter.resource, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(count, barriers);
    if (meter)
    {
        constants.mode = NR_STATS_METER;
        Dispatch(list, g_nr.statsPipeline, meterTable, constants, statsX, statsY);
        UavBarrier(&barriers[0], g_nr.meter.resource);
        list->ResourceBarrier(1, barriers);
        // The resolve eases the white point towards this frame's by the time since the last metered frame; it jumps
        // there on the first frame and when the game says Reset (a cut).
        const double now = LogClock();
        const double since = now - g_nr.meterClock;
        NrConstants resolve = constants;
        resolve.mode = NR_STATS_RESOLVE;
        resolve.seconds = since > 0.0 ? float(since < 60.0 ? since : 60.0) : 0.0f;
        if (g_nr.meterFresh || f.reset != 0)
            resolve.flags |= NR_FLAG_SCENE_RESET;
        g_nr.meterFresh = false;
        g_nr.meterClock = now;
        Dispatch(list, g_nr.statsPipeline, meterTable, resolve, 1, 1);
        Transition(&barriers[0], g_nr.scene.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(&barriers[1], g_nr.meter.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(2, barriers);
    }
    if (stats)
    {
        constants.mode = NR_STATS_CLEAR;
        Dispatch(list, g_nr.statsPipeline, encodeTable, constants, 1, 1);
        UavBarrier(&barriers[0], g_nr.stats.resource);
        list->ResourceBarrier(1, barriers);
    }
    constants.mode = NR_MODE_ENCODE;
    Dispatch(list, g_nr.pipeline, encodeTable, constants, modelX, modelY);
    if (stats)
    {
        constants.mode = NR_STATS_INPUT;
        Dispatch(list, g_nr.statsPipeline, encodeTable, constants, statsX, statsY);
    }

    // 3: the model's inputs. The depth's passes read the game's guides in the NPSR state they come in, before the
    // clones move them.
    if (dilate)
    {
        NrConstants c = constants;
        c.mode = NR_MODE_DILATE;
        c.outWidth = f.motionWidth;
        c.outHeight = f.motionHeight;
        Dispatch(list, g_nr.pipeline, dilateTable, c, Groups(f.motionWidth, 8), Groups(f.motionHeight, 8));
    }
    if (sky)
    {
        NrConstants c = constants;
        c.mode = NR_MODE_SKY;
        c.outWidth = f.guideWidth;
        c.outHeight = f.guideHeight;
        Dispatch(list, g_nr.pipeline, skyTable, c, Groups(f.guideWidth, 8), Groups(f.guideHeight, 8));
    }
    count = 0;
    Transition(&barriers[count++], g_nr.proxy.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (stats)
        UavBarrier(&barriers[count++], g_nr.stats.resource);
    if (dilate)
        Transition(&barriers[count++], g_nr.dilated.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (sky)
        Transition(&barriers[count++], g_nr.mask.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(count, barriers);
    ID3D12Resource* depth = f.depth;
    unsigned depthBaseX = f.depthBaseX, depthBaseY = f.depthBaseY;
    if (g_nr.depthClone.resource != nullptr)
    {
        Clone(list, f.depth, f.depthBaseX, f.depthBaseY, g_nr.depthClone);
        depth = g_nr.depthClone.resource;
        depthBaseX = depthBaseY = 0;
    }
    ID3D12Resource* motion = f.motion;
    unsigned motionBaseX = f.motionBaseX, motionBaseY = f.motionBaseY;
    if (g_nr.motionClone.resource != nullptr)
    {
        Clone(list, f.motion, f.motionBaseX, f.motionBaseY, g_nr.motionClone);
        motion = g_nr.motionClone.resource;
        motionBaseX = motionBaseY = 0;
    }
    if (dilate)
    {
        motion = g_nr.dilated.resource;
        motionBaseX = motionBaseY = 0;
    }

    // 4: the model.
    const unsigned reset = g_nr.resetNext || f.reset != 0 ? 1u : 0u;
    g_nr.resetNext = false;
    FillEvaluate(g_nr.evalParams, f, modelWidth, modelHeight, depth, depthBaseX, depthBaseY, motion, motionBaseX,
                 motionBaseY, reset);
    FillMask(g_nr.evalParams, sky ? &g_nr.mask : nullptr);
    FillTunables(g_nr.evalParams, s.model);
    if (timing)
        list->EndQuery(g_nr.queries, D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestamps + 1);
    const int result = g_nr.bridge.evaluate(list, g_nr.handle, g_nr.evalParams, nullptr);
    if (timing)
        list->EndQuery(g_nr.queries, D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestamps + 2);
    const bool delivered = result == NVSDK_NGX_Result_Success;
    const bool dump = dumpDue && delivered;
    const bool fit = scaled && delivered;

    // 5: the model's output, and with ModelScale the fit over it.
    count = 0;
    Transition(&barriers[count++], g_nr.modelOutput.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (dump)
        Transition(&barriers[count++], g_nr.dump.resource, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(count, barriers);
    if (fit)
    {
        Dispatch(list, g_nr.fitPipeline, fitTable, constants, modelX, modelY);
        Transition(&barriers[0], g_nr.slope.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(&barriers[1], g_nr.value.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(&barriers[2], g_nr.raw.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(3, barriers);
    }

    // 6: the composite, only if the model delivered. The statistics of its change (over the model's picture) and the
    // dump's first half read the Output before it, while it is still in NPSR.
    if (delivered)
    {
        if (stats)
        {
            NrConstants c = constants;
            c.mode = NR_STATS_GAIN;
            c.width = modelWidth;
            c.height = modelHeight;
            Dispatch(list, g_nr.statsPipeline, compositeTable, c, Groups(modelWidth, 32), Groups(modelHeight, 32));
        }
        if (dump)
            DumpPasses(list, dumpTable, constants, NR_MODE_DUMP_BEFORE, layout);
        Transition(&barriers[0], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, barriers);
        if (g_nr.frames.load(std::memory_order_relaxed) == 0)
        {
            const float seconds = s.badgeSeconds;
            g_nr.badgeUntil = seconds > 0.0f ? LogClock() + double(seconds) : 0.0;
            g_nr.badgeSize = f.height / 40 > 8 ? f.height / 40 : 8;
        }
        constants.mode = NR_MODE_COMPOSITE;
        constants.badgeSize = LogClock() < g_nr.badgeUntil ? g_nr.badgeSize : 0;
        Dispatch(list, g_nr.pipeline, compositeTable, constants, groupsX, groupsY);
        constants.badgeSize = 0;
        if (dump)
        {
            Transition(&barriers[0], f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, barriers);
            DumpPasses(list, dumpTable, constants, NR_MODE_DUMP_AFTER, layout);
            Transition(&barriers[0], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, barriers);
        }
        g_nr.frames.fetch_add(1, std::memory_order_relaxed);
        if (g_nr.failRun != 0)
        {
            LogModelRecovered(g_nr.failRun);
            g_nr.failRun = 0;
        }
    }
    else
    {
        Transition(&barriers[0], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, barriers);
        g_nr.failed.fetch_add(1, std::memory_order_relaxed);
        if (g_nr.failRun++ == 0)
            LogModelFailure(result);
        else if (g_nr.failRun >= kFailuresBeforeOff)
            Off("the model failed 600 evaluations in a row");
    }

    // The preview picture for the menu, from the proxy or the model's output while both are still readable. A frame
    // the model did not deliver renders none, so the menu keeps the last.
    if (preview && delivered)
    {
        NrConstants c = constants;
        c.mode = NR_MODE_PREVIEW;
        c.scale = g_nr.previewScale;
        c.outWidth = g_nr.preview.width;
        c.outHeight = g_nr.preview.height;
        if (s.zebra)
            c.flags |= NR_FLAG_ZEBRA;
        if (s.preview == Preview::Output)
            c.flags |= NR_FLAG_PREVIEW_OUTPUT;
        Transition(&barriers[0], g_nr.preview.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, barriers);
        Dispatch(list, g_nr.pipeline, previewTable, c, Groups(c.outWidth, 8), Groups(c.outHeight, 8));
        Transition(&barriers[0], g_nr.preview.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, barriers);
        PublishPicture(frameIndex);
    }

    // The copies the CPU reads a few frames from now.
    if (stats)
    {
        CopyOut(list, g_nr.stats, g_nr.statsReadback, (frameIndex % kStatsSlots) * kStatsSlotBytes);
        g_nr.statsInfo = Info(f, s, source, constants, frameIndex, feature);
        g_nr.statsInfo.modelRan = delivered;
        g_nr.statsForLog = logStats;
        g_nr.statsPending = true;
    }
    if (dump)
    {
        CopyOut(list, g_nr.dump, g_nr.dumpReadback, 0);
        g_nr.dumpInfo = Info(f, s, source, constants, frameIndex, feature);
        g_nr.dumpInfo.modelRan = true;
        g_nr.dumpWanted.store(false, std::memory_order_relaxed);
        g_nr.dumpState.store(kDumpRecorded, std::memory_order_relaxed);
    }

    // 7: back to how they arrived (Output already is).
    count = 0;
    Transition(&barriers[count++], g_nr.proxy.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(&barriers[count++], g_nr.modelOutput.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_nr.depthClone.resource != nullptr)
        Transition(&barriers[count++], g_nr.depthClone.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    if (g_nr.motionClone.resource != nullptr)
        Transition(&barriers[count++], g_nr.motionClone.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    if (meter)
        Transition(&barriers[count++], g_nr.scene.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (dilate)
        Transition(&barriers[count++], g_nr.dilated.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (sky)
        Transition(&barriers[count++], g_nr.mask.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (fit)
    {
        Transition(&barriers[count++], g_nr.slope.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(&barriers[count++], g_nr.value.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(&barriers[count++], g_nr.raw.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    list->ResourceBarrier(count, barriers);
    if (timing)
    {
        list->EndQuery(g_nr.queries, D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestamps + 3);
        list->ResolveQueryData(g_nr.queries, D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestamps, kTimestamps,
                               g_nr.timingReadback.resource, uint64_t(timingSlot) * kTimestamps * 8);
        g_nr.timed[timingSlot] = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The background thread: reads dlssnr.ini again when it changes, asks the render thread for a
// frame's statistics every StatsLog seconds and logs them, asks for a frame dump every DumpEvery seconds and writes
// it. It never takes g_lock and never touches the GPU.

// The value below which `fraction` of the pixels lie, from a histogram of `bins` bins of 1/perEv EV from lowEv, as
// text: EV, or "black" / "<-16" / ">+8" when it falls outside the bins. `black`, `below` and `above` count the pixels
// under everything, under the first bin and over the last.
void Percentile(const uint32_t* hist, unsigned bins, int lowEv, int perEv, uint64_t black, uint64_t below,
                uint64_t above, double fraction, char* out, size_t size)
{
    uint64_t total = black + below + above;
    for (unsigned i = 0; i < bins; ++i)
        total += hist[i];
    if (total == 0)
    {
        snprintf(out, size, "-");
        return;
    }
    const double target = fraction * double(total);
    double seen = double(black);
    if (target < seen)
    {
        snprintf(out, size, "black");
        return;
    }
    seen += double(below);
    if (target < seen)
    {
        snprintf(out, size, "<%+d", lowEv);
        return;
    }
    for (unsigned i = 0; i < bins; ++i)
    {
        if (hist[i] != 0 && target < seen + double(hist[i]))
        {
            const double within = (target - seen) / double(hist[i]);
            snprintf(out, size, "%+.2f", double(lowEv) + (double(i) + within) / double(perEv));
            return;
        }
        seen += double(hist[i]);
    }
    snprintf(out, size, ">%+d", lowEv + int(bins) / perEv);
}

double Share(uint32_t part, uint32_t whole) { return whole != 0 ? 100.0 * double(part) / double(whole) : 0.0; }

const char* const kWhiteSourceText[] = { "exposure", "manual", "scene", "auto" };

void LogSample(const Sample& sample)
{
    const FrameInfo& info = sample.info;
    const uint32_t* w = sample.words;
    const uint32_t* c = w + NR_S_COUNTS;
    const bool linear = (info.flags & NR_FLAG_LINEAR_HDR) != 0;
    const float white = FloatBits(c[NR_C_WHITE]);
    const float largest = FloatBits(c[NR_C_MAX_BITS]);
    const uint32_t pixels = c[NR_C_PIXELS];
    const uint32_t finite = pixels - c[NR_C_NONFINITE];
    // Black pixels are not in the histograms; the ones below and above their range are counted apart.
    const uint64_t black = c[NR_C_BLACK];
    char p[6][16], lum[2][16], gain[3][16];
    const double points[6] = { 0.01, 0.10, 0.50, 0.90, 0.99, 0.999 };
    for (int i = 0; i < 6; ++i)
        Percentile(w + NR_S_MAX_HIST, NR_HIST_BINS, NR_HIST_EV_MIN, NR_HIST_PER_EV, black, c[NR_C_BELOW],
                   c[NR_C_ABOVE], points[i], p[i], sizeof p[i]);
    // The luminance histogram has no counts of its own below and above its range: what is outside it is left out.
    uint32_t lumCounted = 0;
    for (unsigned i = 0; i < NR_HIST_BINS; ++i)
        lumCounted += w[NR_S_LUM_HIST + i];
    const uint64_t lumOutside = finite > lumCounted ? finite - lumCounted : 0;
    Percentile(w + NR_S_LUM_HIST, NR_HIST_BINS, NR_HIST_EV_MIN, NR_HIST_PER_EV, lumOutside, 0, 0, 0.50, lum[0],
               sizeof lum[0]);
    Percentile(w + NR_S_LUM_HIST, NR_HIST_BINS, NR_HIST_EV_MIN, NR_HIST_PER_EV, lumOutside, 0, 0, 0.99, lum[1],
               sizeof lum[1]);
    const double gainPoints[3] = { 0.01, 0.50, 0.99 };
    for (int i = 0; i < 3; ++i)
        Percentile(w + NR_S_GAIN_HIST, NR_GAIN_BINS, NR_GAIN_EV_MIN, NR_GAIN_PER_EV, 0, 0, 0, gainPoints[i], gain[i],
                   sizeof gain[i]);

    char line[1000];
    size_t length = 0;
    auto add = [&](const char* format, auto... args) {
        if (length + 1 < sizeof line)
        {
            const int written = snprintf(line + length, sizeof line - length, format, args...);
            if (written > 0)
                length += size_t(written) < sizeof line - length ? size_t(written) : sizeof line - length - 1;
        }
    };
    add("stats: frame %llu, %s %ux%u, %s", static_cast<unsigned long long>(info.frame),
        info.feature == 13 ? "RR" : "SR", info.width, info.height, linear ? "linear HDR" : "tonemapped");
    if (linear)
    {
        char preExposure[24], exposure[24];
        snprintf(preExposure, sizeof preExposure, info.havePreExposure ? "%.4g" : "absent", double(info.preExposure));
        // "exposure" is the game's exposure texture; with the scene white point, t3 holds our own instead.
        const bool gameExposure = (info.flags & NR_FLAG_EXPOSURE) != 0 && info.whiteSource == WhiteSource::Exposure;
        snprintf(exposure, sizeof exposure, gameExposure ? "%.4g" : "-", double(FloatBits(c[NR_C_EXPOSURE])));
        add(": W %.4g (WhiteSource %s, pre-exposure %s, exposure %s, WhiteEV %+.2f)", double(white),
            kWhiteSourceText[unsigned(info.whiteSource) % 4], preExposure, exposure, double(info.whiteEV));
    }
    add("; largest channel / W in EV: p1 %s, p10 %s, p50 %s, p90 %s, p99 %s, p99.9 %s, max %+.2f (value %.4g); "
        "luminance / W: p50 %s, p99 %s",
        p[0], p[1], p[2], p[3], p[4], p[5], largest > 0.0f && white > 0.0f ? std::log2(double(largest / white)) : -99.0,
        double(largest), lum[0], lum[1]);
    if (linear)
        add("; Shoulder %.2f: in it %.2f%%, compressed >3 EV %.2f%%", double(info.shoulder),
            Share(c[NR_C_SHOULDER], pixels), Share(c[NR_C_HEAVY], pixels));
    add("; 0 in 8 bits %.2f%%, black %.2f%%, negative %.2f%%, not finite %u", Share(c[NR_C_DARK], pixels),
        Share(c[NR_C_BLACK], pixels), Share(c[NR_C_NEGATIVE], pixels), c[NR_C_NONFINITE]);
    if (info.modelRan)
        add("; model change x DetailStrength %.2f in EV: p1 %s, p50 %s, p99 %s, beyond MaxGainEV %.2f: %.2f%% / %.2f%%, "
            "colour changed %.2f%%",
            double(info.detail), gain[0], gain[1], gain[2], double(info.maxGain), Share(c[NR_C_GAIN_LOW], c[NR_C_GAIN_PIXELS]),
            Share(c[NR_C_GAIN_HIGH], c[NR_C_GAIN_PIXELS]), Share(c[NR_C_COLOUR], c[NR_C_GAIN_PIXELS]));
    else
        add("; the model did not deliver this frame");
    add("; settings #%u", info.generation);
    if ((info.flags & NR_FLAG_SCALED) != 0)
        add("; ModelScale: model input %ux%u", info.modelWidth, info.modelHeight);
    Log("%s", line);
}

// The .bzdump file (tools/bzdump.py reads it): this header, then the GPU's block exactly as nr.hlsl wrote it.
struct DumpHeader
{
    char magic[8];          // "BZDUMP1"
    uint32_t headerBytes;   // sizeof(DumpHeader)
    uint32_t blockBytes;    // the GPU block that follows
    uint64_t frame;         // our frame counter
    uint32_t width, height; // the frame
    uint32_t scale, reducedWidth, reducedHeight;
    uint32_t cropWidth, cropHeight, cropX, cropY;
    uint32_t pictures; // NR_DUMP_PICTURES
    uint32_t flags;    // NrConstants.flags
    uint32_t outputFormat; // DXGI_FORMAT
    uint32_t feature;      // 1 SR, 13 RR
    uint32_t generation;   // the settings snapshot
    uint32_t inputType, whiteSource;
    float preExposure; // NaN when the game gave none
    float whiteScale;  // pre-exposure (or 1) x 2^WhiteEV, before the exposure texture divides it
    float whiteEV, shoulder, detail, colour, maxGain, highlight;
    char exe[64]; // the game's executable, without .exe, UTF-8
};
static_assert(sizeof(DumpHeader) == 184, "tools/bzdump.py reads this layout");

// "witcher3" for the file name and the header.
void ExeName(wchar_t* out, size_t size)
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = L"game";
    if (length != 0 && length < MAX_PATH)
    {
        const wchar_t* slash = wcsrchr(path, L'\\');
        name = slash != nullptr ? slash + 1 : path;
        wchar_t* dot = wcsrchr(path, L'.');
        if (dot != nullptr && dot > name)
            *dot = L'\0';
    }
    wcsncpy_s(out, size, name, _TRUNCATE);
}

void WriteDump(unsigned index)
{
    const FrameInfo& info = g_nr.dumpInfo;
    const DumpLayout layout = Layout(info.width, info.height);
    wchar_t folder[MAX_PATH];
    if (!LocalFolder(L"dumps", folder, MAX_PATH))
    {
        Log("NR: no folder for frame dumps under %%LOCALAPPDATA%%\\Banana-Zero; dump dropped");
        return;
    }
    wchar_t exe[64];
    ExeName(exe, 64);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\%ls-%04u%02u%02u-%02u%02u%02u-f%llu.bzdump", folder, exe, unsigned(now.wYear),
               unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour), unsigned(now.wMinute),
               unsigned(now.wSecond), static_cast<unsigned long long>(info.frame));

    DumpHeader header = {};
    memcpy(header.magic, "BZDUMP1", 8);
    header.headerBytes = sizeof header;
    header.blockBytes = uint32_t(layout.bytes);
    header.frame = info.frame;
    header.width = info.width;
    header.height = info.height;
    header.scale = NR_DUMP_SCALE;
    header.reducedWidth = layout.reducedWidth;
    header.reducedHeight = layout.reducedHeight;
    header.cropWidth = layout.cropWidth;
    header.cropHeight = layout.cropHeight;
    header.cropX = layout.cropX;
    header.cropY = layout.cropY;
    header.pictures = NR_DUMP_PICTURES;
    header.flags = info.flags;
    header.outputFormat = uint32_t(info.format);
    header.feature = info.feature;
    header.generation = info.generation;
    header.inputType = unsigned(info.inputType);
    header.whiteSource = unsigned(info.whiteSource);
    header.preExposure = info.havePreExposure ? info.preExposure : std::nanf("");
    header.whiteScale = info.whiteScale;
    header.whiteEV = info.whiteEV;
    header.shoulder = info.shoulder;
    header.detail = info.detail;
    header.colour = info.colour;
    header.maxGain = info.maxGain;
    header.highlight = info.highlight;
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, header.exe, int(sizeof header.exe) - 1, nullptr, nullptr);

    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Log("NR: frame dump %ls not written (%lu)", path, GetLastError());
        return;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, &header, sizeof header, &written, nullptr) && written == sizeof header;
    const uint8_t* data = g_nr.dumpReadback.mapped;
    uint64_t left = layout.bytes;
    while (ok && left != 0)
    {
        const DWORD chunk = DWORD(left < (16u << 20) ? left : (16u << 20));
        ok = WriteFile(file, data, chunk, &written, nullptr) && written == chunk;
        data += chunk;
        left -= chunk;
    }
    CloseHandle(file);
    Log("NR: frame dump %u of at most %u, frame %llu: %ls (%.1f MB)%s", index, kMaxDumps,
        static_cast<unsigned long long>(info.frame), path, double(layout.bytes + sizeof header) / 1048576.0,
        ok ? "" : "; writing it failed");
}

DWORD WINAPI Worker(void*)
{
    double nextStats = 0.0;
    double nextDump = 0.0;
    unsigned dumps = 0;
    bool dumpLimitSaid = false;
    while (!g_nr.off.load(std::memory_order_acquire) || g_nr.dumpState.load(std::memory_order_acquire) == kDumpReady)
    {
        Sleep(250);
        SettingsReloadIfChanged();
        const Settings* s = SettingsCurrent();
        const double now = LogClock();

        if (g_sampleState.load(std::memory_order_acquire) == 1)
        {
            LogSample(g_sample);
            g_sampleState.store(0, std::memory_order_release);
        }
        if (s->statsSeconds > 0.0f)
        {
            if (now >= nextStats)
            {
                g_nr.statsWanted.store(true, std::memory_order_relaxed);
                nextStats = now + double(s->statsSeconds);
            }
        }
        else
        {
            g_nr.statsWanted.store(false, std::memory_order_relaxed);
            nextStats = 0.0;
        }

        int ready = kDumpReady;
        if (g_nr.dumpState.compare_exchange_strong(ready, kDumpWriting, std::memory_order_acquire))
        {
            WriteDump(++dumps);
            g_nr.dumpState.store(kDumpIdle, std::memory_order_release);
        }
        if (s->dumpSeconds > 0.0f && dumps < kMaxDumps)
        {
            // The first a little after dumps are switched on (or NR starts), then every DumpEvery seconds.
            if (nextDump == 0.0)
                nextDump = now + (s->dumpSeconds < 20.0f ? double(s->dumpSeconds) : 20.0);
            if (now >= nextDump)
            {
                g_nr.dumpWanted.store(true, std::memory_order_relaxed);
                nextDump = now + double(s->dumpSeconds);
            }
        }
        else
        {
            g_nr.dumpWanted.store(false, std::memory_order_relaxed);
            nextDump = 0.0;
            if (dumps >= kMaxDumps && !dumpLimitSaid)
            {
                dumpLimitSaid = true;
                Log("NR: %u frame dumps written; no more in this process", dumps);
            }
        }
    }
    return 0;
}

void StartWorker()
{
    if (g_nr.workerStarted)
        return;
    g_nr.workerStarted = true;
    const HANDLE thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
        Log("NR: the background thread did not start (%lu): dlssnr.ini is not read again, no statistics, no dumps",
            GetLastError());
}

// What the menu is told (NrStatus). The render thread writes it under g_statusLock (after g_lock, never the other
// way round); the menu holds the lock only to copy it out.
SRWLOCK g_statusLock = SRWLOCK_INIT;
NrStatusState g_status = {};

void NoteFrame(const Frame& f, unsigned feature)
{
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.width = f.width;
    g_status.height = f.height;
    g_status.feature = feature;
    g_status.format = int(f.outputDesc.Format);
    g_status.hdr = f.hdr;
    g_status.havePreExposure = f.havePreExposure;
    g_status.exposureTexture = f.exposure != nullptr && f.exposureView != DXGI_FORMAT_UNKNOWN;
    g_status.lastEvaluateAt = LogClock();
    ReleaseSRWLockExclusive(&g_statusLock);
}

void PublishStatus()
{
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.attempted = g_nr.attempted.load(std::memory_order_relaxed);
    g_status.off = g_nr.off.load(std::memory_order_relaxed);
    g_status.offReason = g_nr.offReason.load(std::memory_order_relaxed);
    g_status.ready = g_nr.ready;
    g_status.haveFeature = g_nr.handle != nullptr;
    g_status.creates = g_nr.creates;
    g_status.frames = g_nr.frames.load(std::memory_order_relaxed);
    g_status.failed = g_nr.failed.load(std::memory_order_relaxed);
    g_status.evaluatesSeen = g_nr.evaluatesSeen;
    g_status.sinceCreate = g_nr.evaluatesSeen - g_nr.createdAt;
    g_status.linear = g_nr.haveEncodeKey && (g_nr.encodeKey.inputType == InputType::LinearHdr ||
                                             (g_nr.encodeKey.inputType == InputType::Auto && g_status.hdr));
    g_status.modelWidth = g_nr.modelWidth;
    g_status.modelHeight = g_nr.modelHeight;
    g_status.haveTiming = g_nr.samples != 0;
    g_status.gpuMs = g_nr.passMs;
    g_status.modelMs = g_nr.modelMs;
    ReleaseSRWLockExclusive(&g_statusLock);
}

// Under the lock: everything after the cheap checks.
// `game` is filled with what the game has bound on `list` before anything is recorded into it, and `*captured` set;
// the caller binds it again afterwards.
Skip Evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params, const NrSource& source, ListState* game,
              bool* captured)
{
    if (g_nr.off.load(std::memory_order_relaxed))
        return kSkipOff; // turned off between the caller's check and its lock (the game shutting the core down)
    if (!g_nr.settingsLoaded)
    {
        g_nr.settingsLoaded = true;
        wchar_t ini[MAX_PATH];
        if (IniPath(ini, MAX_PATH))
            SettingsLoad(ini);
        StartWorker(); // after the first load: from here on only that thread reads the file
    }
    const Settings* s = SettingsCurrent(); // one snapshot for the whole frame
    // The preview lives only while the menu asks for it: closing the menu frees the picture.
    const bool previewing = PreviewWanted(*s);
    if (g_nr.preview.resource != nullptr && !previewing)
        DropPreview();
    if (params == nullptr)
        return kSkipNoParams;
    Frame frame = {};
    const Skip read = ReadFrame(params, source, &frame);
    if (read != kSkipNone)
    {
        if (read == kSkipGeometry && !g_nr.said[kSkipGeometry].load(std::memory_order_relaxed))
            DescribeFrame(frame);
        return read;
    }
    const DXGI_FORMAT outputView = ViewFormat(frame.outputDesc.Format);
    if (outputView == DXGI_FORMAT_UNKNOWN)
    {
        if (!g_nr.said[kSkipGeometry].load(std::memory_order_relaxed))
            DescribeFrame(frame);
        return kSkipGeometry;
    }
    NoteFrame(frame, source.feature);
    // Everything below may record into the game's list and bind state of its own (ours, the model's).
    if (!ListStateCapture(list, game))
        return kSkipState;
    *captured = true;
    // The frozen frame (M3, freeze.h) goes first, NR on or off: with NR off the frozen picture still replaces the
    // game's Output, so that A/B and the split screen compare the same picture.
    FreezeApply(list, &frame);
    if (!s->enabled)
    {
        g_nr.resetNext = true; // the model's history is stale by the time NR is back
        return kSkipDisabled;
    }

    g_nr.attempted.store(true, std::memory_order_relaxed);
    ++g_nr.evaluatesSeen;
    if (!g_nr.ready)
    {
        if (!Initialise(list))
        {
            Off("initialisation failed, see above");
            return kSkipOff;
        }
        g_nr.ready = true;
    }
    if (!TypedLoadSupported(outputView))
        return kSkipTypedLoad;
    if (g_nr.bound.load(std::memory_order_relaxed))
    {
        if (g_nr.sourceId.load(std::memory_order_relaxed) != source.id)
            return kSkipOtherSource;
    }
    else
    {
        g_nr.sourceId.store(source.id, std::memory_order_relaxed);
        g_nr.bound.store(true, std::memory_order_relaxed);
        g_nr.resetNext = true;
    }

    ReleaseParked();
    CollectStats(previewing);
    CollectDump();
    if (g_nr.handle != nullptr && (frame.width != g_nr.createdWidth || frame.height != g_nr.createdHeight))
    {
        if (g_nr.evaluatesSeen - g_nr.createdAt < kEvaluatesBetweenCreates)
            return kSkipWaiting;
        Log("NR: the frame is now %ux%u (was %ux%u); rebuilding", frame.width, frame.height, g_nr.createdWidth,
            g_nr.createdHeight);
        ParkCurrent();
    }
    if (g_nr.handle == nullptr)
    {
        if (!g_nr.describedFrame)
        {
            g_nr.describedFrame = true;
            DescribeFrame(frame);
        }
        if (!CreateFeature(list, frame, s->model))
            return kSkipOff;
        return kSkipCreated;
    }
    Run(list, frame, *s, source.feature, previewing);
    return kSkipNone;
}
} // namespace

void NrAfterEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params, const NrSource& source)
{
    if (g_nr.off.load(std::memory_order_acquire))
    {
        g_nr.skipped[kSkipOff].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock))
    {
        CountSkip(kSkipBusy, source);
        return;
    }
    static ListState game; // under g_lock
    bool captured = false;
    const Skip skip = Evaluate(list, params, source, &game, &captured);
    if (captured)
        ListStateRestore(list, game);
    if (g_nr.attempted.load(std::memory_order_relaxed))
        PublishStatus();
    ReleaseSRWLockExclusive(&g_lock);
    if (skip != kSkipNone)
        CountSkip(skip, source);
}

void NrSourceReleased(unsigned id)
{
    // No lock: this may run while the render thread is inside NR, and must not wait for it.
    if (!g_nr.attempted.load(std::memory_order_relaxed) || !g_nr.bound.load(std::memory_order_relaxed) ||
        g_nr.sourceId.load(std::memory_order_relaxed) != id)
        return;
    g_nr.bound.store(false, std::memory_order_relaxed);
    Log("NR: the DLSS handle it followed (#%u) was released; the next SR/RR handle takes over", id);
}

namespace
{
// Every feature, live and parked, and every texture. For the game's shutdown of the core: by then it has idled its
// GPU for its own features, and nothing of ours is in flight either, since our commands ride its command lists.
unsigned ReleaseEverything()
{
    unsigned released = 0;
    if (g_nr.handle != nullptr)
    {
        g_nr.bridge.release(g_nr.handle);
        ++released;
    }
    g_nr.handle = nullptr;
    for (Parked& p : g_nr.parked)
    {
        if (p.handle != nullptr)
        {
            g_nr.bridge.release(p.handle);
            ++released;
        }
        for (ID3D12Resource* r : p.resources)
        {
            if (r != nullptr)
                r->Release();
        }
        p = {};
    }
    Texture* const textures[] = { &g_nr.proxy, &g_nr.modelOutput, &g_nr.depthClone, &g_nr.motionClone, &g_nr.preview,
                                  &g_nr.scene, &g_nr.dilated,     &g_nr.mask,       &g_nr.slope,       &g_nr.value,
                                  &g_nr.raw };
    for (Texture* t : textures)
    {
        if (t->resource != nullptr)
            t->resource->Release();
        *t = {};
    }
    FreezeRelease();
    AcquireSRWLockExclusive(&g_previewLock);
    g_preview = {};
    g_preview.generation = ++g_nr.previewGeneration;
    ReleaseSRWLockExclusive(&g_previewLock);
    return released;
}

// The meter's, the statistics', the timing's and the dump's buffers. A dump the background thread is writing out
// keeps its buffers: they are left allocated for the rest of the process rather than pulled from under it.
void ReleaseBuffers()
{
    DropBuffer(&g_nr.meter);
    DropBuffer(&g_nr.stats);
    DropBuffer(&g_nr.statsReadback);
    DropBuffer(&g_nr.timingReadback);
    g_nr.statsPending = false;
    int state = kDumpReady;
    g_nr.dumpState.compare_exchange_strong(state, kDumpIdle);
    state = kDumpRecorded;
    g_nr.dumpState.compare_exchange_strong(state, kDumpIdle);
    if (g_nr.dumpState.load() == kDumpIdle) // stays Idle: only the render thread leaves Idle, and NR is ending
    {
        DropBuffer(&g_nr.dump);
        DropBuffer(&g_nr.dumpReadback);
    }
}

template <typename Object> void Drop(Object** object)
{
    if (*object != nullptr)
        (*object)->Release();
    *object = nullptr;
}
} // namespace

void NrBeforeCoreShutdown()
{
    if (!g_nr.attempted.load(std::memory_order_acquire))
        return; // nothing of ours was ever loaded or created
    AcquireSRWLockExclusive(&g_lock);
    if (g_nr.initAttempt != nullptr)
    {
        // The model was initialised: its features first, then its own shutdown, while the core, NvAPI and the device
        // are all still there. Our two parameter blocks stay allocated.
        const unsigned released = ReleaseEverything();
        const int result = g_nr.bridge.shutdown(g_nr.device);
        Log("NR: the game is shutting the NGX core down; %u feature%s released, model Shutdown1 -> 0x%08X %s",
            released, released == 1 ? "" : "s", unsigned(result), ResultName(result));
    }
    // Off before the buffers go, so that the background thread stops asking for statistics and dumps.
    if (!g_nr.off.load(std::memory_order_relaxed))
        Off("the game shut the NGX core down");
    ReleaseBuffers();
    Drop(&g_nr.queries);
    Drop(&g_nr.pipeline);
    Drop(&g_nr.statsPipeline);
    Drop(&g_nr.fitPipeline);
    Drop(&g_nr.rootSignature);
    Drop(&g_nr.heap);
    Drop(&g_nr.device);
    g_nr.ready = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void NrCoreGone()
{
    // Under the loader lock, so nothing here waits on g_lock or calls the model; the flags are read on the next frame.
    g_nr.coreGone.store(true, std::memory_order_release);
    if (g_nr.attempted.load(std::memory_order_relaxed) && !g_nr.off.load(std::memory_order_relaxed))
        Off("the NGX core was unloaded; its parameter blocks went with it");
}

size_t NrDescribe(char* out, size_t size)
{
    if (!g_nr.attempted.load(std::memory_order_relaxed))
    {
        const uint64_t disabled = g_nr.skipped[kSkipDisabled].load(std::memory_order_relaxed);
        if (disabled == 0)
            return 0;
        const int written = snprintf(out, size, "NR disabled (Enabled=0), %llu evaluations passed by",
                                     static_cast<unsigned long long>(disabled));
        return written > 0 ? (size_t(written) < size ? size_t(written) : size - 1) : 0;
    }
    uint64_t skipped = 0;
    for (unsigned i = 1; i < kSkipCount; ++i)
        skipped += g_nr.skipped[i].load(std::memory_order_relaxed);
    const char* reason = g_nr.offReason.load(std::memory_order_relaxed);
    const int written = snprintf(out, size, "NR %llu frames, %llu skipped, %llu failed%s%s",
                                 static_cast<unsigned long long>(g_nr.frames.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(skipped),
                                 static_cast<unsigned long long>(g_nr.failed.load(std::memory_order_relaxed)),
                                 reason != nullptr ? "; off: " : "", reason != nullptr ? reason : "");
    return written > 0 ? (size_t(written) < size ? size_t(written) : size - 1) : 0;
}

bool NrStatus(NrStatusState* out)
{
    g_nr.timingAskedAt.store(LogClock(), std::memory_order_relaxed);
    AcquireSRWLockShared(&g_statusLock);
    *out = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    return out->attempted;
}

bool NrPreview(NrPreviewState* out)
{
    g_nr.previewAskedAt.store(LogClock(), std::memory_order_relaxed);
    AcquireSRWLockShared(&g_previewLock);
    *out = g_preview;
    ReleaseSRWLockShared(&g_previewLock);
    return out->picture != nullptr;
}
