// nrprobe: does the real NR model accept our bridge, our Init_Ext arguments and our own parameter blocks, and does
// it produce a picture? A standalone console program: no game, no injection, nothing written to a game directory.
// Needs the NVIDIA driver, an RTX 50 and the user's copy of nvngx_dlssnr.dll (read only).
//
//   build\Release\nrprobe.exe [--model <path\nvngx_dlssnr.dll>] [--size WxH] [--evaluates N] [--init N]
//                             [--capability] [--no-model-shutdown] [--no-core-shutdown] [--nvapi-first]
//                             [--tuning] [--dumps <folder>] [--subrects] [--captures <folder>]
//
// It makes the same calls in the same order as dxgi.dll's NR pass (src/nr_dx12.cpp), one line of output per step,
// and stops at the first step that fails: device, the driver's core and its Init, the bridge from beside this exe,
// the model, the Init_Ext ladder, the parameter blocks, CreateFeature on a command list, then N
// evaluations over a synthetic frame with the output read back and compared with the input. --init N runs only the
// Nth attempt of the ladder (1..5). --capability uses the core's capability block for everything instead of
// AllocateParameters. The exit code is 0 when the feature was created and every evaluation succeeded.
//
// The teardown is the same as dxgi.dll's when a game shuts NGX down: ReleaseFeature, the model's own Shutdown1
// through the bridge, then the core's Shutdown1. Left to its DLL_PROCESS_DETACH instead, the model's clean-up
// faulted inside NvAPI at process exit; the three switches vary the teardown to show what the fault
// depends on: --no-model-shutdown skips the model's Shutdown1, --no-core-shutdown skips the core's, --nvapi-first
// has this exe initialise NvAPI before anything else and never unload it, as a game does. The verdict line (PASS or
// FAIL) is printed before any shutdown; whether the process then exits cleanly is what its exit code shows.
//
// --tuning then measures, on the same feature, what the model does with its parameters, its motion vectors and its
// control mask (Tuning, below: T0 to T8): that its six parameters act when written at every evaluation and not when
// written only at creation, the model's defaults, its clamps, which motion-vector scale it wants, on Witcher 3 frames
// from dxgi.dll's own dumps (--dumps, else %LOCALAPPDATA%\Banana-Zero\dumps) how each setting moves its colour,
// whether motion vectors dilated by depth help it along moving edges and thin bars, and whether a control mask sets
// it per pixel. The exit code is then also 1 when one of its checks fails.
//
// --subrects measures what a smaller model input needs (Subrects9 and Subrects10, below: T9 and T10): whether the
// model runs on smaller Color and Output subrects inside the feature the basic run made, what that costs, whether
// its picture is a feature's of that size, and which motion-vector scale it wants when its picture is smaller than
// the frame the motion vectors describe.
//
// --captures measures the Witcher 3 colour drift on the files of the capture build of dxgi.dll (Captures, below): per
// capture, whether its frame, dump and finished picture agree, how the game's picture differs from our proxy, how
// much bluer or warmer the model makes the picture given our proxy, the game's own picture, our proxy with the game's
// tone curve, and our proxy with other exposures and settings, and what a colour lock would leave of that; then the
// range of each across the captures, which is the drift.
//
// dxgi.dll from the same folder is loaded too (this exe imports dxgi), so its dlssnr.log appears beside it; it sees
// no SR/RR evaluation here and does nothing.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "nvsdk_ngx.h"

namespace
{
void Say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int Fail(const char* format, ...)
{
    std::fputs("FAIL: ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 1;
}

std::string Utf8(const std::wstring& text)
{
    std::string out(text.size() * 3 + 1, '\0');
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    out.resize(length > 0 ? size_t(length) : 0);
    return out;
}

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
    default: return "?";
    }
}

template <typename Function> Function Proc(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ExeFolder()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder(path, length);
    return folder.substr(0, folder.find_last_of(L'\\'));
}

// ---------------------------------------------------------------------------------------------------------------
// NGX's own log lines, capped.

unsigned g_ngxLines = 0;

void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    if (++g_ngxLines > 200)
        return;
    size_t length = message != nullptr ? std::strlen(message) : 0;
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        --length;
    Say("    ngx[f%d l%d] %.*s", int(feature), int(level), int(length), message != nullptr ? message : "");
}

// ---------------------------------------------------------------------------------------------------------------
// D3D12: the NVIDIA adapter, a device, one direct queue, one command list, a fence.

struct Gpu
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
};

Gpu g;

bool CreateGpu()
{
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g.factory))))
        return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; g.factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            g.adapter = adapter;
            LARGE_INTEGER umd = {};
            adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
            Say("adapter: %s, device 0x%04X, %llu MB, user-mode driver %u.%u.%u.%u", Utf8(desc.Description).c_str(),
                desc.DeviceId, static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), HIWORD(umd.HighPart),
                LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            break;
        }
        adapter->Release();
    }
    if (g.adapter == nullptr)
    {
        Say("no NVIDIA adapter");
        return false;
    }
    if (FAILED(D3D12CreateDevice(g.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device))))
        return false;
    D3D12_COMMAND_QUEUE_DESC queue = {};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&g.queue))) ||
        FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr,
                                           IID_PPV_ARGS(&g.list))) ||
        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        return false;
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.event != nullptr;
}

// Runs what is recorded and waits for it. Milliseconds from submit to completion, or a negative number on failure.
// The list comes back open.
double Submit(const char* what)
{
    HRESULT hr = g.list->Close();
    if (FAILED(hr))
    {
        Say("%s: closing the command list failed 0x%08lX", what, static_cast<unsigned long>(hr));
        return -1.0;
    }
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    if (g.fence->GetCompletedValue() < g.fenceValue)
    {
        g.fence->SetEventOnCompletion(g.fenceValue, g.event);
        if (WaitForSingleObject(g.event, 15000) != WAIT_OBJECT_0)
        {
            Say("%s: the GPU did not finish within 15 s", what);
            return -1.0;
        }
    }
    QueryPerformanceCounter(&end);
    hr = g.device->GetDeviceRemovedReason();
    if (hr != S_OK)
    {
        Say("%s: device removed, reason 0x%08lX", what, static_cast<unsigned long>(hr));
        return -1.0;
    }
    g.allocator->Reset();
    g.list->Reset(g.allocator, nullptr);
    return double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

ID3D12Resource* MakeTexture(DXGI_FORMAT format, unsigned width, unsigned height, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state)
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
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

ID3D12Resource* MakeBuffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    g.list->ResourceBarrier(1, &barrier);
}

// Records a copy of `rows` (rowBytes each, tightly packed) into a texture that is in COPY_DEST, leaving it in
// `after`. The staging buffer is kept until the process ends.
bool Upload(ID3D12Resource* texture, const void* rows, unsigned rowBytes, unsigned height, D3D12_RESOURCE_STATES after)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ID3D12Resource* staging = MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (staging == nullptr)
        return false;
    uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(mapped + footprint.Offset + UINT64(y) * footprint.Footprint.RowPitch,
                    static_cast<const uint8_t*>(rows) + size_t(y) * rowBytes, rowBytes);
    staging->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = texture;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
    return true;
}

// Reads a texture back (it is in `state`, and is again on return). Rows of `rowPitch` bytes.
bool Readback(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, std::vector<uint8_t>* out, unsigned* rowPitch)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ID3D12Resource* buffer = MakeBuffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    if (buffer == nullptr)
        return false;
    Barrier(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = buffer;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (Submit("readback") < 0.0)
        return false;
    uint8_t* mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    out->assign(mapped + footprint.Offset, mapped + footprint.Offset + UINT64(footprint.Footprint.RowPitch) * desc.Height);
    buffer->Unmap(0, nullptr);
    buffer->Release();
    *rowPitch = footprint.Footprint.RowPitch;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Half floats, for the RGBA16F textures.

uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
    {
        if (exponent < -10)
            return uint16_t(sign);
        mantissa |= 0x800000u;
        return uint16_t(sign | (mantissa >> unsigned(14 - exponent)));
    }
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = uint32_t(half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7F800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// The core: where it is, and its exports.

std::wstring FindCore()
{
    wchar_t path[MAX_PATH * 2];
    if (HMODULE umd = GetModuleHandleW(L"nvwgf2umx.dll"); umd != nullptr && GetModuleFileNameW(umd, path, MAX_PATH * 2))
    {
        std::wstring core(path);
        core = core.substr(0, core.find_last_of(L'\\')) + L"\\_nvngx.dll";
        if (FileExists(core))
            return core;
    }
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                     nullptr, value, &size) == ERROR_SUCCESS)
    {
        std::wstring core(value);
        if (core.size() < 4 || _wcsicmp(core.c_str() + core.size() - 4, L".dll") != 0)
            core += L"\\_nvngx.dll";
        if (FileExists(core))
            return core;
    }
    return L"";
}

using CoreInitExt = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int,
                                               const NVSDK_NGX_FeatureCommonInfo*);
using GetParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using Shutdown1 = NVSDK_NGX_Result(__cdecl*)(ID3D12Device*);

using BzLoad = int(__cdecl*)(const wchar_t*);
using BzInit = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
using BzPopulate = int(__cdecl*)(void*);
using BzCreate = int(__cdecl*)(void*, int, void*, void**);
using BzEvaluate = int(__cdecl*)(void*, const void*, void*, void*);
using BzRelease = int(__cdecl*)(void*);
using BzShutdown = int(__cdecl*)(void*);
using BzBuild = const char*(__cdecl*)();

// %LOCALAPPDATA%\Banana-Zero\ngx, created; empty if that cannot be.
std::wstring OurDataPath()
{
    wchar_t local[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return L"";
    std::wstring path = std::wstring(local) + L"\\Banana-Zero";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\ngx";
    CreateDirectoryW(path.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? path : L"";
}

struct Options
{
    std::wstring model;
    unsigned width = 1920;
    unsigned height = 1080;
    unsigned evaluates = 5;
    int init = 0; // 0: the whole ladder
    bool capability = false;
    bool modelShutdown = true;
    bool coreShutdown = true;
    bool nvapiFirst = false;
    bool tuning = false;
    bool subrects = false;
    std::wstring dumps;    // --tuning's T6: where the frame dumps are; empty: %LOCALAPPDATA%\Banana-Zero\dumps
    std::wstring captures; // --captures: the capture build's folder (debug on the desktop)
};

bool ParseOptions(int argc, wchar_t** argv, Options* options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--model" && hasValue)
            options->model = argv[++i];
        else if (arg == L"--size" && hasValue)
        {
            wchar_t* end = nullptr;
            options->width = unsigned(wcstoul(argv[++i], &end, 10));
            options->height = end != nullptr && *end == L'x' ? unsigned(wcstoul(end + 1, nullptr, 10)) : 0;
            if (options->width == 0 || options->height == 0)
                return false;
        }
        else if (arg == L"--evaluates" && hasValue)
            options->evaluates = unsigned(wcstoul(argv[++i], nullptr, 10));
        else if (arg == L"--init" && hasValue)
            options->init = int(wcstoul(argv[++i], nullptr, 10));
        else if (arg == L"--capability")
            options->capability = true;
        else if (arg == L"--no-model-shutdown")
            options->modelShutdown = false;
        else if (arg == L"--no-core-shutdown")
            options->coreShutdown = false;
        else if (arg == L"--nvapi-first")
            options->nvapiFirst = true;
        else if (arg == L"--tuning")
            options->tuning = true;
        else if (arg == L"--subrects")
            options->subrects = true;
        else if (arg == L"--dumps" && hasValue)
            options->dumps = argv[++i];
        else if (arg == L"--captures" && hasValue)
            options->captures = argv[++i];
        else
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// --tuning: what the model does with its parameters and its motion vectors, measured on the GPU (Tuning, below).

// Uploads through one staging buffer, kept and grown: each upload is run and waited for before it returns, so the
// next may reuse the buffer. The texture goes from `before` to COPY_DEST and on to `after`.
ID3D12Resource* g_staging = nullptr;
UINT64 g_stagingSize = 0;

bool UploadNow(ID3D12Resource* texture, const void* rows, unsigned rowBytes, unsigned height,
               D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    if (total > g_stagingSize)
    {
        if (g_staging != nullptr)
            g_staging->Release();
        g_staging = MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        g_stagingSize = g_staging != nullptr ? total : 0;
        if (g_staging == nullptr)
            return false;
    }
    uint8_t* mapped = nullptr;
    if (FAILED(g_staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(mapped + footprint.Offset + UINT64(y) * footprint.Footprint.RowPitch,
                    static_cast<const uint8_t*>(rows) + size_t(y) * rowBytes, rowBytes);
    g_staging->Unmap(0, nullptr);
    if (before != D3D12_RESOURCE_STATE_COPY_DEST)
        Barrier(texture, before, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = g_staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = texture;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
    return Submit("upload") >= 0.0;
}

// A texture made and filled, left in NON_PIXEL_SHADER_RESOURCE; nullptr on failure.
ID3D12Resource* MakeFilled(DXGI_FORMAT format, unsigned width, unsigned height, const void* rows, unsigned rowBytes)
{
    ID3D12Resource* texture =
        MakeTexture(format, width, height, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (texture != nullptr && !UploadNow(texture, rows, rowBytes, height, D3D12_RESOURCE_STATE_COPY_DEST,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        texture->Release();
        return nullptr;
    }
    return texture;
}

ID3D12Resource* MakeOutput(unsigned width, unsigned height)
{
    return MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The model's entry points through the bridge, and the core's AllocateParameters (nullptr with --capability).
struct Model
{
    BzPopulate populate = nullptr;
    BzCreate create = nullptr;
    BzEvaluate evaluate = nullptr;
    BzRelease release = nullptr;
    GetParams allocate = nullptr;
};

// The six parameters the model reads at every evaluation, each with the type it reads it as; the defaults are the
// model's own (what it takes for a key that is absent), as dxgi.dll writes them (nr_dx12.cpp, FillTunables).
struct Tunables
{
    float intensity = 1.0f;
    unsigned style = 0;
    float localStructure = 1.0f;
    float localTone = 1.0f;
    float skin = -1.0f;
    int autoMask = 0;
};

void SetTunables(NVSDK_NGX_Parameter* p, const Tunables& t)
{
    p->Set("DLSSNR.Intensity", t.intensity);
    p->Set("DLSSNR.Style", t.style);
    p->Set("DLSSNR.LocalStructureStrength", t.localStructure);
    p->Set("DLSSNR.LocalToneStrength", t.localTone);
    p->Set("DLSSNR.SkinStructureStrength", t.skin);
    p->Set("DLSSNR.UseAutoMask", t.autoMask);
}

// What one evaluation is given, the way dxgi.dll gives it (nr_dx12.cpp, FillEvaluate).
struct Inputs
{
    unsigned width = 0, height = 0; // Color and Output
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* depth = nullptr;
    unsigned depthWidth = 0, depthHeight = 0;
    ID3D12Resource* motion = nullptr;
    unsigned motionWidth = 0, motionHeight = 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    unsigned depthInverted = 0;
    ID3D12Resource* output = nullptr;
};

void SetInputs(NVSDK_NGX_Parameter* p, const Inputs& in, unsigned reset)
{
    p->Set("DLSSNR.Color", in.colour);
    p->Set("DLSSNR.Depth", in.depth);
    p->Set("DLSSNR.MVec", in.motion);
    p->Set("DLSSNR.Output", in.output);
    p->Set("DLSSNR.Enabled", 1u);
    p->Set("DLSSNR.Width", in.width);
    p->Set("DLSSNR.Height", in.height);
    p->Set("DLSSNR.DepthInverted", in.depthInverted);
    p->Set("DLSSNR.Reset", reset);
    p->Set("DLSSNR.ColorSubrectBaseX", 0u);
    p->Set("DLSSNR.ColorSubrectBaseY", 0u);
    p->Set("DLSSNR.ColorSubrectWidth", in.width);
    p->Set("DLSSNR.ColorSubrectHeight", in.height);
    p->Set("DLSSNR.OutputSubrectBaseX", 0u);
    p->Set("DLSSNR.OutputSubrectBaseY", 0u);
    p->Set("DLSSNR.OutputSubrectWidth", in.width);
    p->Set("DLSSNR.OutputSubrectHeight", in.height);
    p->Set("DLSSNR.DepthSubrectBaseX", 0u);
    p->Set("DLSSNR.DepthSubrectBaseY", 0u);
    p->Set("DLSSNR.DepthSubrectWidth", in.depthWidth);
    p->Set("DLSSNR.DepthSubrectHeight", in.depthHeight);
    p->Set("DLSSNR.MVecSubrectBaseX", 0u);
    p->Set("DLSSNR.MVecSubrectBaseY", 0u);
    p->Set("DLSSNR.MVecSubrectWidth", in.motionWidth);
    p->Set("DLSSNR.MVecSubrectHeight", in.motionHeight);
    p->Set("DLSSNR.MVecScaleX", in.mvScaleX);
    p->Set("DLSSNR.MVecScaleY", in.mvScaleY);
}

// A feature of its own for a test, from a fresh pair of parameter blocks (left allocated, as dxgi.dll leaves its
// own). `atCreate`, when given, is written into the creation block as well. nullptr on failure, said why.
void* CreateOwn(const Model& m, unsigned width, unsigned height, const Tunables* atCreate,
                NVSDK_NGX_Parameter** evalBlock)
{
    NVSDK_NGX_Parameter* create = nullptr;
    NVSDK_NGX_Parameter* evaluate = nullptr;
    if (m.allocate == nullptr || m.allocate(&create) != NVSDK_NGX_Result_Success ||
        m.allocate(&evaluate) != NVSDK_NGX_Result_Success || create == nullptr || evaluate == nullptr)
    {
        Say("    no parameter blocks of our own for another feature");
        return nullptr;
    }
    m.populate(create);
    m.populate(evaluate);
    create->Set("DLSSNR.Enabled", 1u);
    create->Set("DLSSNR.Width", width);
    create->Set("DLSSNR.Height", height);
    create->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    create->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    if (atCreate != nullptr)
        SetTunables(create, *atCreate);
    void* handle = nullptr;
    const int r = m.create(g.list, 18, create, &handle);
    const double ms = Submit("create");
    if (r != NVSDK_NGX_Result_Success || handle == nullptr || ms < 0.0)
    {
        Say("    CreateFeature(%ux%u) -> 0x%08X %s", width, height, unsigned(r), ResultName(r));
        return nullptr;
    }
    *evalBlock = evaluate;
    return handle;
}

bool EvaluateOnce(const Model& m, void* handle, NVSDK_NGX_Parameter* p, const Inputs& in, unsigned reset)
{
    SetInputs(p, in, reset);
    const int r = m.evaluate(g.list, handle, p, nullptr);
    const double ms = Submit("evaluate");
    if (r != NVSDK_NGX_Result_Success || ms < 0.0)
    {
        Say("    EvaluateFeature -> 0x%08X %s%s", unsigned(r), ResultName(r), ms < 0.0 ? ", GPU failed" : "");
        return false;
    }
    return true;
}

// A texture's RGB as floats; RGBA16F textures only.
bool ReadRgb(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, unsigned width, unsigned height,
             std::vector<float>* rgb)
{
    std::vector<uint8_t> raw;
    unsigned pitch = 0;
    if (!Readback(texture, state, &raw, &pitch))
        return false;
    rgb->resize(size_t(width) * height * 3);
    for (unsigned y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(raw.data() + size_t(y) * pitch);
        for (unsigned x = 0; x < width; ++x)
            for (unsigned c = 0; c < 3; ++c)
                (*rgb)[(size_t(y) * width + x) * 3 + c] = HalfToFloat(row[x * 4 + c]);
    }
    return true;
}

// `frames` evaluations of the same inputs from a reset, the tunables written before each (none when nullptr), the
// last output read back.
bool Run(const Model& m, void* handle, NVSDK_NGX_Parameter* p, const Inputs& in, const Tunables* t, unsigned frames,
         std::vector<float>* out)
{
    for (unsigned i = 0; i < frames; ++i)
    {
        if (t != nullptr)
            SetTunables(p, *t);
        if (!EvaluateOnce(m, handle, p, in, i == 0 ? 1u : 0u))
            return false;
    }
    return ReadRgb(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, in.width, in.height, out);
}

// Mean absolute difference over every channel of every pixel; NaN and infinity count as 1.
double Difference(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size())
        return 1.0e9;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double d = std::fabs(double(a[i]) - double(b[i]));
        sum += d == d && d < 65504.0 ? d : 1.0;
    }
    return sum / double(a.size());
}

double SrgbDecode(double e) { return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4); }

// How much bluer (+) or warmer (-) `after` is than `before`, as the mean change of log2(blue / red), over the pixels
// of rows y0..y1 that are neither near black nor clipped, and over the warm ones among them (red > green > blue,
// saturated: grass, skin, wood). Both display-encoded RGB, as the model sees and makes them.
struct Shift
{
    double all = 0.0;
    double warm = 0.0;
    double warmShare = 0.0;
};

// The sums behind a Shift, one pixel at a time, in linear light (decoded).
struct ShiftSum
{
    double all = 0.0, warm = 0.0;
    unsigned long long nAll = 0, nWarm = 0;

    void Add(double pr, double pg, double pb, double orr, double og, double ob)
    {
        const double eps = 1.0e-3;
        const double yp = 0.2126 * pr + 0.7152 * pg + 0.0722 * pb;
        const double pmax = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
        const double pmin = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
        const double omax = orr > og ? (orr > ob ? orr : ob) : (og > ob ? og : ob);
        if (!(yp > 0.01) || pmax >= 0.97 || !(omax < 0.97) || !(orr >= 0.0) || !(ob >= 0.0))
            return;
        const double d = std::log2((ob + eps) / (orr + eps)) - std::log2((pb + eps) / (pr + eps));
        all += d;
        ++nAll;
        if (pr > pg && pg > pb && (pmax - pmin) / pmax > 0.15)
        {
            warm += d;
            ++nWarm;
        }
    }

    Shift Result() const
    {
        Shift s;
        s.all = nAll != 0 ? all / double(nAll) : 0.0;
        s.warm = nWarm != 0 ? warm / double(nWarm) : 0.0;
        s.warmShare = nAll != 0 ? double(nWarm) / double(nAll) : 0.0;
        return s;
    }
};

// `step` 2 looks at every other pixel of every other row: enough for a full-size frame.
Shift ColourShift(const std::vector<float>& before, const std::vector<float>& after, unsigned width, unsigned y0,
                  unsigned y1, unsigned step = 1)
{
    ShiftSum sum;
    for (unsigned y = y0; y < y1; y += step)
    {
        for (unsigned x = 0; x < width; x += step)
        {
            const size_t i = (size_t(y) * width + x) * 3;
            sum.Add(SrgbDecode(before[i]), SrgbDecode(before[i + 1]), SrgbDecode(before[i + 2]), SrgbDecode(after[i]),
                    SrgbDecode(after[i + 1]), SrgbDecode(after[i + 2]));
        }
    }
    return sum.Result();
}

// A frame dump from dxgi.dll (nr_dx12.cpp, DumpHeader; tools in the M4 docs read the same): the whole frame reduced
// four times, as the game made it (linear for a linear HDR frame), as the model saw it (the proxy) and as it came
// back (the model's output), the last two display-encoded RGB; the white point the proxy was made with (the block's
// second word), the shoulder and the pass's flags.
struct Dump
{
    unsigned width = 0, height = 0;
    std::vector<float> frame, proxy, model;
    float white = 1.0f, shoulder = 0.7f;
    uint32_t flags = 0;
};

bool ReadDump(const std::wstring& path, Dump* d)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return false;
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) != 0;)
        data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    auto word = [&](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    };
    if (data.size() < 184 || std::memcmp(data.data(), "BZDUMP1", 8) != 0)
        return false;
    const uint32_t headerBytes = word(8), blockBytes = word(12);
    const uint32_t rw = word(36), rh = word(40), cw = word(44), ch = word(48), pictures = word(60);
    const uint64_t pictureBytes = uint64_t(rw) * rh * 8;
    if (pictures < 3 || rw == 0 || rh == 0 || uint64_t(headerBytes) + blockBytes > data.size() ||
        64 + uint64_t(pictures) * (uint64_t(rw) * rh + uint64_t(cw) * ch) * 8 + 4 > blockBytes)
        return false;
    const uint8_t* block = data.data() + headerBytes;
    d->width = rw;
    d->height = rh;
    d->flags = word(64);
    std::memcpy(&d->shoulder, data.data() + 100, 4);
    std::memcpy(&d->white, block + 4, 4);
    for (int which = 0; which <= 2; ++which)
    {
        std::vector<float>& out = which == 0 ? d->frame : which == 1 ? d->proxy : d->model;
        out.resize(size_t(rw) * rh * 3);
        const uint8_t* picture = block + 64 + pictureBytes * unsigned(which);
        for (size_t i = 0; i < size_t(rw) * rh; ++i)
        {
            for (unsigned c = 0; c < 3; ++c)
            {
                uint16_t half;
                std::memcpy(&half, picture + i * 8 + c * 2, 2);
                out[i * 3 + c] = HalfToFloat(half);
            }
        }
    }
    return true;
}

// RGB floats as RGBA16F rows, alpha 1.
std::vector<uint16_t> ToHalfRgba(const std::vector<float>& rgb, size_t from, size_t pixels)
{
    std::vector<uint16_t> out(pixels * 4);
    for (size_t i = 0; i < pixels; ++i)
    {
        for (unsigned c = 0; c < 3; ++c)
            out[i * 4 + c] = FloatToHalf(rgb[(from + i) * 3 + c]);
        out[i * 4 + 3] = FloatToHalf(1.0f);
    }
    return out;
}

// The moving card for T5: detail at several sizes and in colour, display-encoded values around the middle.
float Card(int x, int y, unsigned c)
{
    const float checker = (((x >> 2) + (y >> 2)) & 1) != 0 ? 0.07f : -0.07f;
    const float wave = 0.16f * std::sin(float(x) * 0.21f + float(c)) * std::cos(float(y) * 0.17f - float(c));
    const float ramp = 0.12f * std::sin(float(x) * 0.013f + float(y) * 0.007f + float(c) * 2.1f);
    return 0.45f + wave + ramp + checker;
}

uint32_t g_random = 0x9E3779B9u;
float Noise() // -1 .. 1
{
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return float(g_random) / 2147483648.0f - 1.0f;
}

int g_mustFail = 0; // --tuning checks that failed

void Expect(bool ok, const char* format, ...)
{
    std::fputs(ok ? "  ok    " : "  FAIL  ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (!ok)
        ++g_mustFail;
}

void Note(const char* format, ...)
{
    std::fputs("  info  ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// T5: the card scrolls kSpeed pixels a frame to the right with fresh noise every frame. The motion vectors are a
// texture at 0.58 of the frame's size (as the games give theirs) holding the motion in UV units, -kSpeed / width,
// and MVecScale is scanned as f times the frame's size: the games give f = 0.58 (their render size), and if the
// model reads the motion as MVec x MVecScale / its frame's size, f = 1 is right. With the right scale the model's
// history lines up with the card and the noise averages out; with another it smears. E is the mean |output - the
// clean card| over the middle of the frame and the last four frames.
void Tuning5(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& base)
{
    constexpr unsigned kFrames = 16;
    constexpr int kSpeed = 6;
    constexpr float kNoise = 0.06f;
    constexpr unsigned kMargin = 128;
    const unsigned w = base.width, h = base.height;
    if (w <= 2 * kMargin || h <= 2 * kMargin)
    {
        Note("T5 skipped: the frame is too small");
        return;
    }
    const unsigned mw = unsigned(std::lround(0.58 * w)), mh = unsigned(std::lround(0.58 * h));
    const uint16_t zero = FloatToHalf(0.0f), one = FloatToHalf(1.0f);
    std::vector<uint16_t> moving(size_t(mw) * mh * 2, zero), still(size_t(mw) * mh * 2, zero);
    for (size_t i = 0; i < size_t(mw) * mh; ++i)
        moving[i * 2] = FloatToHalf(-float(kSpeed) / float(w));
    ID3D12Resource* motion = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, mw, mh, moving.data(), mw * 4);
    ID3D12Resource* noMotion = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, mw, mh, still.data(), mw * 4);
    ID3D12Resource* colour =
        MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (motion == nullptr || noMotion == nullptr || colour == nullptr)
    {
        Note("T5 skipped: could not make its textures");
        return;
    }
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_COPY_DEST;

    // The card once, wide enough for every shift: column x of frame t is column x - t kSpeed of the card.
    const unsigned pad = kSpeed * kFrames;
    const unsigned cw = w + pad;
    std::vector<float> card(size_t(cw) * h * 3);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < cw; ++x)
            for (unsigned c = 0; c < 3; ++c)
                card[(size_t(y) * cw + x) * 3 + c] = Card(int(x) - int(pad), int(y), c);
    std::vector<uint16_t> frame(size_t(w) * h * 4);
    std::vector<float> out;
    const Tunables defaults;

    // One run: E over the last four frames, or a negative number when something failed.
    auto run = [&](int speed, bool withMotion, float f, bool resetEach) -> double {
        g_random = 0x9E3779B9u; // the same noise in every run
        Inputs in = base;
        in.colour = colour;
        in.motion = withMotion ? motion : noMotion;
        in.motionWidth = mw;
        in.motionHeight = mh;
        in.mvScaleX = f * float(w);
        in.mvScaleY = f * float(h);
        double sum = 0.0;
        unsigned counted = 0;
        for (unsigned t = 0; t < kFrames; ++t)
        {
            const unsigned offset = pad - unsigned(speed) * t; // the card's column for x = 0
            for (unsigned y = 0; y < h; ++y)
            {
                const float* row = &card[(size_t(y) * cw + offset) * 3];
                uint16_t* to = &frame[size_t(y) * w * 4];
                for (unsigned x = 0; x < w; ++x)
                {
                    for (unsigned c = 0; c < 3; ++c)
                        to[x * 4 + c] = FloatToHalf(row[x * 3 + c] + kNoise * Noise());
                    to[x * 4 + 3] = one;
                }
            }
            if (!UploadNow(colour, frame.data(), w * 8, h, colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                return -1.0;
            colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            SetTunables(evalBlock, defaults);
            if (!EvaluateOnce(m, handle, evalBlock, in, t == 0 || resetEach ? 1u : 0u))
                return -1.0;
            if (t + 4 < kFrames)
                continue;
            if (!ReadRgb(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, w, h, &out))
                return -1.0;
            double e = 0.0;
            size_t n = 0;
            for (unsigned y = kMargin; y + kMargin < h; ++y)
            {
                const float* clean = &card[(size_t(y) * cw + offset) * 3];
                for (unsigned x = kMargin; x + kMargin < w; ++x)
                {
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const double d = std::fabs(double(out[(size_t(y) * w + x) * 3 + c]) - double(clean[x * 3 + c]));
                        e += d == d ? d : 1.0;
                        ++n;
                    }
                }
            }
            sum += e / double(n);
            ++counted;
        }
        return counted != 0 ? sum / double(counted) : -1.0;
    };

    Say("T5 motion vectors: the card scrolls %d px a frame, noise +-%.2f, motion vectors %ux%u in UV units, %u "
        "frames a run",
        kSpeed, double(kNoise), mw, mh, kFrames);
    const double eReset = run(kSpeed, true, 1.0f, true);
    const double eStill = run(0, false, 0.0f, false);
    Note("T5 no history (Reset every frame) E %.5f; the card standing still E %.5f", eReset, eStill);
    const float factors[] = { -1.0f, 0.0f, 0.58f, 0.79f, 1.0f, 1.25f, 1.72f };
    double e[7] = {};
    int best = -1;
    for (int i = 0; i < 7; ++i)
    {
        e[i] = run(kSpeed, true, factors[i], false);
        Note("T5 MVecScale = %+.2f x frame size: E %.5f%s", double(factors[i]), e[i],
             i == 2 ? "  (what the games give)" : i == 4 ? "  (the frame's own size)" : "");
        if (e[i] >= 0.0 && (best < 0 || e[i] < e[best]))
            best = i;
    }
    motion->Release();
    noMotion->Release();
    colour->Release();
    if (eReset < 0.0 || eStill < 0.0 || best < 0)
    {
        Note("T5 verdict: runs failed (above)");
        return;
    }
    const double help = eReset - eStill;
    const double gap = help * 0.1 > 3.0e-4 ? help * 0.1 : 3.0e-4;
    if (help <= 1.0e-3)
        Note("T5 verdict: inconclusive, the model's history barely changes its output on this card (%.5f)", help);
    else if (best == 4 && e[4] + gap < e[2])
        Note("T5 verdict: the model wants the motion in units of its own frame: at the games' scale it follows only "
             "0.58 of it (E %.5f against %.5f). Scale MVecScale by frame size / motion-vector size",
             e[2], e[4]);
    else if (e[2] <= e[4] + gap)
        Note("T5 verdict: the games' scale is as good as any (E %.5f, the frame's own size %.5f): leave it", e[2], e[4]);
    else
        Note("T5 verdict: the best is f = %+.2f (E %.5f), neither the games' scale nor the frame's size: look closer",
             double(factors[best]), e[best]);
}

// T6: Witcher 3 frames as dxgi.dll dumped them (DumpEvery): the proxy, what the model saw in the game, given to a
// feature of the dump's reduced size, and per setting how much bluer (+) or warmer (-) the model makes the picture,
// in log2 of blue over red, over the whole frame and over its warm pixels, next to what the model did in the game.
// Then the bottom half alone: if the sky above it is what cools the ground, the ground alone comes back warmer.
void Tuning6(const Model& m, const std::wstring& folderGiven)
{
    std::wstring folder = folderGiven;
    if (folder.empty())
    {
        wchar_t local[MAX_PATH];
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            Note("T6 skipped: no %%LOCALAPPDATA%%");
            return;
        }
        folder = std::wstring(local) + L"\\Banana-Zero\\dumps";
    }
    if (m.allocate == nullptr)
    {
        Note("T6 skipped: --capability has no parameter blocks of our own for more features");
        return;
    }
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW found;
    const HANDLE find = FindFirstFileW((folder + L"\\witcher3-*.bzdump").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
            files.push_back(found.cFileName);
        while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (files.empty())
    {
        Note("T6 skipped: no Witcher 3 frame dumps in %s", Utf8(folder).c_str());
        return;
    }
    std::sort(files.begin(), files.end());
    if (files.size() > 8)
        files.erase(files.begin(), files.end() - 8); // the newest eight: the names carry the time

    struct Case
    {
        const char* name;
        Tunables t;
    };
    std::vector<Case> cases;
    Tunables t;
    cases.push_back({ "model defaults", t });
    t = Tunables();
    t.style = 1;
    cases.push_back({ "Style 1 (natural)", t });
    t = Tunables();
    t.style = 2;
    cases.push_back({ "Style 2 (cinematic)", t });
    t = Tunables();
    t.localTone = 0.0f;
    cases.push_back({ "LocalTone 0", t });
    t = Tunables();
    t.localTone = 0.5f;
    cases.push_back({ "LocalTone 0.5", t });
    t = Tunables();
    t.localStructure = 0.0f;
    cases.push_back({ "LocalStructure 0", t });
    t = Tunables();
    t.intensity = 0.5f;
    cases.push_back({ "Intensity 0.5", t });
    t = Tunables();
    t.autoMask = 1;
    cases.push_back({ "AutoMask 1", t });
    constexpr unsigned kFrames = 8;

    // A feature over `rows` rows of the proxy from row `top`, run with each of `count` cases; outputs kept.
    auto replay = [&](const Dump& d, unsigned top, unsigned rows, size_t count, std::vector<std::vector<float>>* outs) {
        const unsigned w = d.width;
        const std::vector<uint16_t> rgba = ToHalfRgba(d.proxy, size_t(top) * w, size_t(w) * rows);
        const std::vector<float> flat(size_t(w) * rows, 0.5f);
        const std::vector<uint16_t> still(size_t(w) * rows * 2, FloatToHalf(0.0f));
        Inputs in;
        in.width = in.depthWidth = in.motionWidth = w;
        in.height = in.depthHeight = in.motionHeight = rows;
        in.mvScaleX = float(w);
        in.mvScaleY = float(rows);
        in.colour = MakeFilled(DXGI_FORMAT_R16G16B16A16_FLOAT, w, rows, rgba.data(), w * 8);
        in.depth = MakeFilled(DXGI_FORMAT_R32_FLOAT, w, rows, flat.data(), w * 4);
        in.motion = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, w, rows, still.data(), w * 4);
        in.output = MakeOutput(w, rows);
        NVSDK_NGX_Parameter* p = nullptr;
        void* feature = in.colour != nullptr && in.depth != nullptr && in.motion != nullptr && in.output != nullptr
                            ? CreateOwn(m, w, rows, nullptr, &p)
                            : nullptr;
        outs->assign(count, std::vector<float>());
        bool ok = feature != nullptr;
        for (size_t i = 0; ok && i < count; ++i)
            ok = Run(m, feature, p, in, &cases[i].t, kFrames, &(*outs)[i]);
        if (feature != nullptr)
            m.release(feature);
        for (ID3D12Resource* r : { in.colour, in.depth, in.motion, in.output })
            if (r != nullptr)
                r->Release();
        return ok;
    };

    for (const std::wstring& file : files)
    {
        Dump d;
        const std::string name = Utf8(file);
        if (!ReadDump(folder + L"\\" + file, &d))
        {
            Note("T6 %s: not a frame dump this probe can read", name.c_str());
            continue;
        }
        const unsigned w = d.width, h = d.height, top = h / 2;
        const Shift game = ColourShift(d.proxy, d.model, w, 0, h);
        Say("T6 %s, %ux%u: change of log2(blue / red), whole frame | warm pixels (%.0f%% of the frame)", name.c_str(),
            w, h, 100.0 * game.warmShare);
        Note("T6   %-34s %+.3f | %+.3f", "in the game (the dump's own output)", game.all, game.warm);
        std::vector<std::vector<float>> outs;
        if (!replay(d, 0, h, cases.size(), &outs))
        {
            Note("T6   the replay failed (above)");
            continue;
        }
        for (size_t i = 0; i < cases.size(); ++i)
        {
            const Shift s = ColourShift(d.proxy, outs[i], w, 0, h);
            Note("T6   %-34s %+.3f | %+.3f", cases[i].name, s.all, s.warm);
        }
        std::vector<std::vector<float>> alone;
        if (!replay(d, top, h - top, 1, &alone))
        {
            Note("T6   the bottom half alone failed (above)");
            continue;
        }
        const std::vector<float> bottom(d.proxy.begin() + std::ptrdiff_t(size_t(top) * w * 3), d.proxy.end());
        const Shift inFrame = ColourShift(d.proxy, outs[0], w, top, h);
        const Shift byItself = ColourShift(bottom, alone[0], w, 0, h - top);
        Note("T6   bottom half, model defaults: in the whole frame %+.3f | %+.3f, alone %+.3f | %+.3f", inFrame.all,
             inFrame.warm, byItself.all, byItself.warm);
    }
}

// What T7 moves over the card: its own pattern, finer and brighter, so a smear of either into the other shows.
float Square(int x, int y, unsigned c)
{
    const float stripes = (((x / 3) + (y / 3)) & 1) != 0 ? 0.08f : -0.08f;
    const float wave =
        0.18f * std::sin(float(x) * 0.37f + 1.3f * float(c)) * std::sin(float(y) * 0.29f - 0.7f * float(c));
    return 0.55f + wave + stripes;
}

// The two things T7 moves: a solid square, and a fence of thin bars across the same square (grass and hair are
// where motion vectors at 0.58 of the frame's size miss most). E is measured in three regions: the first is where
// the games' vectors are wrong, so where a dilation can win; the second is where a dilation spreads the mover's
// motion onto the card, so where it can lose (for the square that happens inside the first region already).
struct Mover
{
    unsigned bar, period; // 0, 0: solid; else bars `bar` pixels wide every `period` pixels
    const char* first;    // the first two regions, for the verdict
    const char* second;
};

struct Regions
{
    double e[3] = {};
};

void Verdict7(const Mover& mover, const Regions& games, const Regions& dilated, const Regions& perPixel)
{
    constexpr double kNoticeable = 3.0e-4;
    const double possible = games.e[0] - perPixel.e[0];
    const double gain = games.e[0] - dilated.e[0];
    const double cost = dilated.e[1] - games.e[1];
    if (gain <= -kNoticeable)
        Note("T7 verdict: the dilation makes it worse %s: E %.5f -> %.5f (per-pixel vectors %.5f)", mover.first,
             games.e[0], dilated.e[0], perPixel.e[0]);
    else if (possible <= kNoticeable && gain <= kNoticeable)
        Note("T7 verdict: per-pixel vectors barely beat the games' %s (%.5f): nothing for a dilation to win",
             mover.first, possible);
    else if (gain >= kNoticeable && gain >= 0.25 * possible && cost >= kNoticeable)
        Note("T7 verdict: the dilation helps %s (E %.5f -> %.5f) but costs %s (E %.5f -> %.5f)", mover.first,
             games.e[0], dilated.e[0], mover.second, games.e[1], dilated.e[1]);
    else if (gain >= kNoticeable && gain >= 0.25 * possible)
        Note("T7 verdict: the dilation helps %s: E %.5f -> %.5f (%.0f%% of what per-pixel vectors gain)", mover.first,
             games.e[0], dilated.e[0], possible > 0.0 ? 100.0 * gain / possible : 100.0);
    else
        Note("T7 verdict: the dilation makes no clear difference %s (E %.5f -> %.5f; per-pixel vectors %.5f)",
             mover.first, games.e[0], dilated.e[0], perPixel.e[0]);
}

// T7: motion vectors dilated by depth. Something nearer than the card moves kSpeed pixels a frame to the right over
// it, with fresh noise every frame: a solid square, then a fence of thin bars across the same square. Depth and
// motion vectors come at 0.58 of the frame's size, as the games give them, and a texel takes the mover's motion and
// depth where its centre falls on the mover, so along its edges some of its pixels get the card's motion (and the
// other way round) and the model pulls their history from the wrong place; a bar not two texels wide is all edge.
// The DLL has a dilation of its own (each pixel takes the motion of the nearest of five depth taps in a cross) but
// never hands it the depth (the teardown, and T4), so the probe dilates the same way on the CPU and hands over the
// result. Runs per mover: the vectors as the games give them; dilated; at the frame's own size, each pixel with its
// own motion (the best there is); and Reset every frame (no history). E as in T5, over the last four frames: for the
// square within kBand pixels of its edges (on either side), inside it, on the card away from it; for the bars on
// them, between them (and up to kBand pixels outside the fence), on the card away from them.
void Tuning7(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& base)
{
    constexpr unsigned kFrames = 16;
    constexpr unsigned kSpeed = 8;
    constexpr float kNoise = 0.06f;
    constexpr unsigned kSide = 320;
    constexpr unsigned kBand = kSpeed; // a pixel can see the other side of the edge in its history this far from it
    constexpr unsigned kMargin = 16;
    constexpr float kNear = 0.2f, kFar = 0.9f; // DepthInverted 0: smaller is nearer
    const unsigned w = base.width, h = base.height;
    const unsigned left0 = w / 6, top = h > kSide ? (h - kSide) / 2 : 0;
    if (left0 + kSide + kSpeed * kFrames + kBand + kMargin >= w || top < kBand + kMargin)
    {
        Note("T7 skipped: the frame is too small");
        return;
    }
    const unsigned rw = unsigned(std::lround(0.58 * w)), rh = unsigned(std::lround(0.58 * h));
    const uint16_t zero = FloatToHalf(0.0f), one = FloatToHalf(1.0f), moving = FloatToHalf(-float(kSpeed) / float(w));
    const Mover movers[] = { { 0, 0, "along the edges", "inside the square" },
                             { 2, 12, "on the bars", "between the bars" } };

    // The card and the mover's pattern once; the frames are cut from them.
    std::vector<float> card(size_t(w) * h * 3), square(size_t(kSide) * kSide * 3);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            for (unsigned c = 0; c < 3; ++c)
                card[(size_t(y) * w + x) * 3 + c] = Card(int(x), int(y), c);
    for (unsigned y = 0; y < kSide; ++y)
        for (unsigned x = 0; x < kSide; ++x)
            for (unsigned c = 0; c < 3; ++c)
                square[(size_t(y) * kSide + x) * 3 + c] = Square(int(x), int(y), c);
    // Whether the point (x, y), in frame pixels, is on the mover when its left edge is at `left`. A pixel is on it
    // when its centre is, as a texel of the guides is.
    auto on = [&](const Mover& mover, double x, double y, double left) {
        if (x < left || x >= left + kSide || y < double(top) || y >= double(top + kSide))
            return false;
        return mover.period == 0 || std::fmod(x - left, double(mover.period)) < double(mover.bar);
    };
    auto clean = [&](const Mover& mover, unsigned x, unsigned y, unsigned left, unsigned c) {
        return on(mover, x + 0.5, y + 0.5, left) ? square[(size_t(y - top) * kSide + (x - left)) * 3 + c]
                                                 : card[(size_t(y) * w + x) * 3 + c];
    };

    // The guides of frame t at width gw x gh: texel (i, j) is the mover's when its centre, in frame pixels, is on
    // the mover. `dilate` then gives each texel the motion of the nearest of itself and its four neighbours.
    std::vector<float> depthTexels;
    std::vector<uint16_t> motionTexels, dilated;
    auto guides = [&](const Mover& mover, unsigned t, unsigned gw, unsigned gh, bool dilate) {
        const double left = double(left0 + kSpeed * t);
        depthTexels.assign(size_t(gw) * gh, kFar);
        motionTexels.assign(size_t(gw) * gh * 2, zero);
        for (unsigned j = 0; j < gh; ++j)
        {
            const double cy = (j + 0.5) * double(h) / double(gh);
            for (unsigned i = 0; i < gw; ++i)
            {
                if (on(mover, (i + 0.5) * double(w) / double(gw), cy, left))
                {
                    depthTexels[size_t(j) * gw + i] = kNear;
                    motionTexels[(size_t(j) * gw + i) * 2] = moving;
                }
            }
        }
        if (!dilate)
            return;
        dilated = motionTexels;
        for (unsigned j = 0; j < gh; ++j)
        {
            for (unsigned i = 0; i < gw; ++i)
            {
                size_t best = size_t(j) * gw + i;
                const size_t taps[4] = { size_t(j) * gw + (i > 0 ? i - 1 : i),
                                         size_t(j) * gw + (i + 1 < gw ? i + 1 : i), size_t(j > 0 ? j - 1 : j) * gw + i,
                                         size_t(j + 1 < gh ? j + 1 : j) * gw + i };
                for (size_t tap : taps)
                    if (depthTexels[tap] < depthTexels[best])
                        best = tap;
                dilated[(size_t(j) * gw + i) * 2] = motionTexels[best * 2];
                dilated[(size_t(j) * gw + i) * 2 + 1] = motionTexels[best * 2 + 1];
            }
        }
        motionTexels.swap(dilated);
    };

    ID3D12Resource* colour =
        MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    guides(movers[0], 0, rw, rh, false);
    ID3D12Resource* depthLow = MakeFilled(DXGI_FORMAT_R32_FLOAT, rw, rh, depthTexels.data(), rw * 4);
    ID3D12Resource* motionLow = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, rw, rh, motionTexels.data(), rw * 4);
    guides(movers[0], 0, w, h, false);
    ID3D12Resource* depthFull = MakeFilled(DXGI_FORMAT_R32_FLOAT, w, h, depthTexels.data(), w * 4);
    ID3D12Resource* motionFull = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, w, h, motionTexels.data(), w * 4);
    if (colour == nullptr || depthLow == nullptr || motionLow == nullptr || depthFull == nullptr ||
        motionFull == nullptr)
    {
        Note("T7 skipped: could not make its textures");
        for (ID3D12Resource* r : { colour, depthLow, motionLow, depthFull, motionFull })
            if (r != nullptr)
                r->Release();
        return;
    }
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_COPY_DEST;
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::vector<uint16_t> frame(size_t(w) * h * 4);
    std::vector<float> out;
    const Tunables defaults;

    // One run: E per region over the last four frames; false when something failed.
    enum Guides
    {
        kGames,
        kDilated,
        kFull
    };
    auto run = [&](const Mover& mover, Guides which, bool resetEach, Regions* e) -> bool {
        g_random = 0x9E3779B9u; // the same noise in every run
        const bool full = which == kFull;
        const unsigned gw = full ? w : rw, gh = full ? h : rh;
        Inputs in = base;
        in.colour = colour;
        in.depth = full ? depthFull : depthLow;
        in.motion = full ? motionFull : motionLow;
        in.depthWidth = in.motionWidth = gw;
        in.depthHeight = in.motionHeight = gh;
        in.mvScaleX = float(gw); // the vectors are in UV units; the scale makes them texels, as the games give it
        in.mvScaleY = float(gh);
        in.depthInverted = 0;
        Regions sum;
        unsigned counted = 0;
        for (unsigned t = 0; t < kFrames; ++t)
        {
            const unsigned left = left0 + kSpeed * t;
            for (unsigned y = 0; y < h; ++y)
            {
                uint16_t* to = &frame[size_t(y) * w * 4];
                for (unsigned x = 0; x < w; ++x)
                {
                    for (unsigned c = 0; c < 3; ++c)
                        to[x * 4 + c] = FloatToHalf(clean(mover, x, y, left, c) + kNoise * Noise());
                    to[x * 4 + 3] = one;
                }
            }
            guides(mover, t, gw, gh, which == kDilated);
            if (!UploadNow(colour, frame.data(), w * 8, h, colourState, kRead) ||
                !UploadNow(in.depth, depthTexels.data(), gw * 4, gh, kRead, kRead) ||
                !UploadNow(in.motion, motionTexels.data(), gw * 4, gh, kRead, kRead))
                return false;
            colourState = kRead;
            SetTunables(evalBlock, defaults);
            if (!EvaluateOnce(m, handle, evalBlock, in, t == 0 || resetEach ? 1u : 0u))
                return false;
            if (t + 4 < kFrames)
                continue;
            if (!ReadRgb(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, w, h, &out))
                return false;
            double region[3] = {};
            size_t count[3] = {};
            for (unsigned y = kMargin; y + kMargin < h; ++y)
            {
                const bool rowNear = y + kBand >= top && y < top + kSide + kBand;
                const bool rowDeep = y >= top + kBand && y + kBand < top + kSide;
                for (unsigned x = kMargin; x + kMargin < w; ++x)
                {
                    const bool nearMover = rowNear && x + kBand >= left && x < left + kSide + kBand;
                    unsigned r = 2;
                    if (mover.period == 0)
                    {
                        const bool deepInside = rowDeep && x >= left + kBand && x + kBand < left + kSide;
                        r = deepInside ? 1 : nearMover ? 0 : 2;
                    }
                    else
                        r = on(mover, x + 0.5, y + 0.5, left) ? 0 : nearMover ? 1 : 2;
                    double d = 0.0;
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const double diff =
                            std::fabs(double(out[(size_t(y) * w + x) * 3 + c]) - double(clean(mover, x, y, left, c)));
                        d += diff == diff ? diff : 1.0;
                    }
                    region[r] += d;
                    count[r] += 3;
                }
            }
            for (unsigned r = 0; r < 3; ++r)
                sum.e[r] += count[r] != 0 ? region[r] / double(count[r]) : 0.0;
            ++counted;
        }
        for (unsigned r = 0; r < 3; ++r)
            e->e[r] = sum.e[r] / counted;
        return true;
    };

    Say("T7 depth-dilated motion vectors: guides %ux%u, %u frames a run, noise +-%.2f, the mover %u px a frame to the "
        "right; E over the last four frames",
        rw, rh, kFrames, double(kNoise), kSpeed);
    for (const Mover& mover : movers)
    {
        if (mover.period == 0)
            Say("T7 a %ux%u square; E within %u px of its edges | inside it | on the card away from it", kSide, kSide,
                kBand);
        else
            Say("T7 bars %u px wide every %u px across the same square; E on the bars | between them (and up to %u px "
                "outside) | on the card away from them",
                mover.bar, mover.period, kBand);
        Regions reset, games, dilatedE, perPixel;
        if (!run(mover, kGames, true, &reset) || !run(mover, kGames, false, &games) ||
            !run(mover, kDilated, false, &dilatedE) || !run(mover, kFull, false, &perPixel))
        {
            Note("T7 verdict: runs failed (above)");
            break;
        }
        const struct
        {
            const char* name;
            const Regions& e;
        } rows[] = { { "no history (Reset every frame)", reset },
                     { "the games' vectors (0.58 size)", games },
                     { "the same, dilated by depth", dilatedE },
                     { "per-pixel vectors (frame size)", perPixel } };
        for (const auto& row : rows)
            Note("T7 %-32s E %.5f | %.5f | %.5f", row.name, row.e.e[0], row.e.e[1], row.e.e[2]);
        Verdict7(mover, games, dilatedE, perPixel);
    }
    for (ID3D12Resource* r : { colour, depthLow, motionLow, depthFull, motionFull })
        r->Release();
}

// The control mask (DLSSNR.ControlMask) for the next evaluations: `mask` over its whole `width` x `height`, or none.
void SetMask(NVSDK_NGX_Parameter* p, ID3D12Resource* mask, unsigned width, unsigned height)
{
    p->Set("DLSSNR.ControlMask", mask);
    p->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
    p->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
    p->Set("DLSSNR.ControlMaskSubrectWidth", width);
    p->Set("DLSSNR.ControlMaskSubrectHeight", height);
}

// One texel of a control mask: .x blend (times Intensity, the final blend between the frame and the model's
// picture), .y tone (times LocalTone), .z structure (times LocalStructure); .w is unused.
struct MaskValue
{
    float blend, tone, structure;
};

// T8: the control mask, the one input that sets the model per pixel. By the teardown (its section 10), each pixel's
// tone and structure strengths are the sliders' times the mask's .y and .z, the final blend is Intensity times .x,
// the model's own skin mask is off while a mask is given, and the mask is stretched over the frame whatever its size.
// On the basic run's frame with the defaults: a constant mask against the slider it should equal (and how far that
// slider moves the picture); whether values above 1 go further than the sliders' 1, and whether the sliders do;
// how far a change of the mask reaches: the right half at tone 0 and structure 0, each half against the whole frame
// at its own setting, by distance from the split; a mask at half the frame's size against the full-size one; and
// that the picture is the same as before once the mask is taken away. Informational.
void Tuning8(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& in,
             const std::vector<float>& base, double same, double differs)
{
    constexpr unsigned kFrames = 6; // as the other tuning runs
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const unsigned w = in.width, h = in.height, hw = w / 2, hh = h / 2;
    ID3D12Resource* full =
        MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* half =
        MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, hw, hh, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (full == nullptr || half == nullptr || hw < 64)
    {
        Note("T8 skipped: could not make the mask textures");
        for (ID3D12Resource* r : { full, half })
            if (r != nullptr)
                r->Release();
        return;
    }
    D3D12_RESOURCE_STATES fullState = D3D12_RESOURCE_STATE_COPY_DEST, halfState = D3D12_RESOURCE_STATE_COPY_DEST;
    std::vector<uint16_t> texels(size_t(w) * h * 4);
    const Tunables defaults;

    // One run with the full-size or the half-size mask, `leftOf` left of its texel column `column` and `rest` from
    // there on.
    auto run = [&](bool useHalf, unsigned column, MaskValue leftOf, MaskValue rest, const Tunables& t,
                   std::vector<float>* out) {
        ID3D12Resource* mask = useHalf ? half : full;
        D3D12_RESOURCE_STATES& state = useHalf ? halfState : fullState;
        const unsigned mw = useHalf ? hw : w, mh = useHalf ? hh : h;
        const uint16_t a[4] = { FloatToHalf(leftOf.blend), FloatToHalf(leftOf.tone), FloatToHalf(leftOf.structure),
                                FloatToHalf(1.0f) };
        const uint16_t b[4] = { FloatToHalf(rest.blend), FloatToHalf(rest.tone), FloatToHalf(rest.structure),
                                FloatToHalf(1.0f) };
        for (unsigned y = 0; y < mh; ++y)
            for (unsigned x = 0; x < mw; ++x)
                std::memcpy(&texels[(size_t(y) * mw + x) * 4], x < column ? a : b, sizeof a);
        if (!UploadNow(mask, texels.data(), mw * 8, mh, state, kRead))
            return false;
        state = kRead;
        SetMask(evalBlock, mask, mw, mh);
        const bool ran = Run(m, handle, evalBlock, in, &t, kFrames, out);
        SetMask(evalBlock, nullptr, 0, 0);
        return ran;
    };
    auto constant = [&](MaskValue v, const Tunables& t, std::vector<float>* out) {
        return run(false, 0, v, v, t, out);
    };
    auto label = [&](double d) { return d <= same ? "the same" : d >= differs ? "differs" : "close"; };

    Say("T8 control mask: a constant mask against the slider it should equal (mask vs slider | the slider vs the "
        "defaults)");
    const MaskValue neutral = { 1.0f, 1.0f, 1.0f };
    std::vector<float> neutralOut, masked, slider;
    if (!constant(neutral, defaults, &neutralOut))
    {
        Note("T8 skipped: the run with a neutral mask failed (above)");
        for (ID3D12Resource* r : { full, half })
            r->Release();
        return;
    }
    const double neutralVsNone = Difference(neutralOut, base);
    Note("T8 %-40s %.6f %s", "(1, 1, 1) vs no mask", neutralVsNone, label(neutralVsNone));
    struct Case
    {
        const char* what;
        MaskValue mask;
        Tunables t;
    };
    std::vector<Case> cases;
    Tunables t = defaults;
    t.localTone = 0.0f;
    cases.push_back({ "(1, 0, 1) vs LocalTone 0", { 1.0f, 0.0f, 1.0f }, t });
    t = defaults;
    t.localStructure = 0.5f;
    cases.push_back({ "(1, 1, 0.5) vs LocalStructure 0.5", { 1.0f, 1.0f, 0.5f }, t });
    t = defaults;
    t.intensity = 0.5f;
    cases.push_back({ "(0.5, 1, 1) vs Intensity 0.5", { 0.5f, 1.0f, 1.0f }, t });
    for (const Case& c : cases)
    {
        if (!constant(c.mask, defaults, &masked) || !Run(m, handle, evalBlock, in, &c.t, kFrames, &slider))
        {
            Note("T8 %s: a run failed (above)", c.what);
            continue;
        }
        const double d = Difference(masked, slider);
        Note("T8 %-40s %.6f %s | %.6f", c.what, d, label(d), Difference(slider, base));
    }
    std::vector<float> beyond;
    t = defaults;
    t.localTone = 2.0f;
    t.localStructure = 2.0f;
    if (constant({ 1.0f, 2.0f, 2.0f }, defaults, &beyond) && Run(m, handle, evalBlock, in, &t, kFrames, &slider))
        Note("T8 above 1: the mask's (1, 2, 2) vs (1, 1, 1) %.6f %s; LocalTone and LocalStructure 2 vs 1 %.6f %s; the "
             "two %.6f %s",
             Difference(beyond, neutralOut), label(Difference(beyond, neutralOut)), Difference(slider, base),
             label(Difference(slider, base)), Difference(beyond, slider), label(Difference(beyond, slider)));

    // The split: how far into the left half the right half's setting reaches, and the other way round. Each half is
    // compared with a mask of its own value over the whole frame.
    constexpr unsigned kBands = 4;
    const unsigned bandEdge[kBands + 1] = { 0, 8, 32, 128, 1u << 30 };
    const MaskValue flatValue = { 1.0f, 0.0f, 0.0f };
    std::vector<float> split, flat, splitHalf;
    if (!run(false, w / 2, neutral, flatValue, defaults, &split) || !constant(flatValue, defaults, &flat))
        Note("T8 the split: a run failed (above)");
    else
    {
        const double signal = Difference(neutralOut, flat);
        Say("T8 split: the right half at tone 0 and structure 0 (which moves the whole frame %.6f); each half against "
            "the whole frame at its own setting, by distance from the split: 0-8 | 8-32 | 32-128 | 128+ px",
            signal);
        double off[2][kBands] = {};
        for (unsigned side = 0; side < 2; ++side)
        {
            const std::vector<float>& own = side == 0 ? neutralOut : flat;
            for (unsigned band = 0; band < kBands; ++band)
            {
                double sum = 0.0;
                size_t n = 0;
                for (unsigned y = 0; y < h; ++y)
                {
                    for (unsigned x = 0; x < w; ++x)
                    {
                        const unsigned distance =
                            side == 0 ? (x < w / 2 ? w / 2 - 1 - x : ~0u) : (x >= w / 2 ? x - w / 2 : ~0u);
                        if (distance < bandEdge[band] || distance >= bandEdge[band + 1])
                            continue;
                        for (unsigned c = 0; c < 3; ++c)
                        {
                            const size_t i = (size_t(y) * w + x) * 3 + c;
                            sum += std::fabs(double(split[i]) - double(own[i]));
                        }
                        n += 3;
                    }
                }
                off[side][band] = n != 0 ? sum / double(n) : 0.0;
            }
            Note("T8 %-36s %.6f | %.6f | %.6f | %.6f",
                 side == 0 ? "left, against (1, 1, 1) everywhere" : "right, against (1, 0, 0) everywhere", off[side][0],
                 off[side][1], off[side][2], off[side][3]);
        }
        if (run(true, hw / 2, neutral, flatValue, defaults, &splitHalf))
            Note("T8 the same split in a mask at half the size: %.6f from the full-size one %s",
                 Difference(splitHalf, split), label(Difference(splitHalf, split)));
        const double reach = std::max(std::max(off[0][2], off[0][3]), std::max(off[1][2], off[1][3]));
        if (signal < differs)
            Note("T8 verdict: tone 0 and structure 0 hardly move this frame (%.6f), so the split shows nothing",
                 signal);
        else if (reach <= 0.1 * signal)
            Note("T8 verdict: the mask acts locally: from 32 px off the split each half is within %.0f%% of what the "
                 "setting moves",
                 100.0 * reach / signal);
        else
            Note("T8 verdict: the mask reaches far: 32 to 128 px off the split the halves are off their own settings "
                 "by %.0f%% and %.0f%% of what the setting moves, beyond 128 px by %.0f%% and %.0f%%",
                 100.0 * off[0][2] / signal, 100.0 * off[1][2] / signal, 100.0 * off[0][3] / signal,
                 100.0 * off[1][3] / signal);
    }

    std::vector<float> after;
    if (Run(m, handle, evalBlock, in, &defaults, kFrames, &after))
        Note("T8 the mask taken away again: %.6f from before %s", Difference(after, base),
             label(Difference(after, base)));
    for (ID3D12Resource* r : { full, half })
        r->Release();
}

// --subrects: the model at a smaller input than the feature was created for, as dxgi.dll runs it when ModelScale is
// below 100%: the Color and Output subrects shrink, the feature and its textures stay. The teardown says the model
// sizes its network from the subrects at every evaluation (and ScalingRatio is held at 1), so a smaller subrect should
// cost less without a new feature.
//
// T9: at 100%, 75%, 66.7%, 50% and 33.3% of the basic run's frame, inside the basic run's textures: whether the model
// takes the subrect, whether it leaves the Output outside it alone, whether the picture is the one a feature created at
// that size makes of the same pixels, and how long an evaluation takes (median, wall time from submit to fence).
void Subrects9(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& base)
{
    constexpr unsigned kWarm = 5;
    constexpr unsigned kTimed = 30;
    const unsigned w = base.width, h = base.height;
    const uint16_t sentinel[4] = { FloatToHalf(0.25f), FloatToHalf(0.5f), FloatToHalf(0.75f), FloatToHalf(1.0f) };
    std::vector<uint16_t> fill(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        std::memcpy(&fill[i * 4], sentinel, sizeof sentinel);
    const Tunables defaults;
    struct Size
    {
        const char* name;
        unsigned num, den;
    };
    const Size sizes[] = { { "100%", 1, 1 }, { "75%", 3, 4 }, { "66.7%", 2, 3 }, { "50%", 1, 2 }, { "33.3%", 1, 3 } };
    double fullMs = 0.0;
    Say("T9 subrects: the feature was created at %ux%u; each size runs %u evaluations from a Reset, the last %u timed",
        w, h, kWarm + kTimed, kTimed);
    for (const Size& size : sizes)
    {
        const unsigned sw = (w * size.num + size.den / 2) / size.den, sh = (h * size.num + size.den / 2) / size.den;
        if (!UploadNow(base.output, fill.data(), w * 8, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            Note("T9 %s: could not fill the Output", size.name);
            continue;
        }
        Inputs in = base;
        in.width = sw;
        in.height = sh;
        std::vector<double> times;
        double firstMs = 0.0; // the evaluation right after the change of size
        bool ok = true;
        for (unsigned i = 0; i < kWarm + kTimed && ok; ++i)
        {
            SetTunables(evalBlock, defaults);
            SetInputs(evalBlock, in, i == 0 ? 1u : 0u);
            const int r = m.evaluate(g.list, handle, evalBlock, nullptr);
            const double ms = Submit("evaluate");
            if (r != NVSDK_NGX_Result_Success || ms < 0.0)
            {
                Note("T9 %s (%ux%u in %ux%u): EvaluateFeature -> 0x%08X %s%s", size.name, sw, sh, w, h, unsigned(r),
                     ResultName(r), ms < 0.0 ? ", GPU failed" : "");
                ok = false;
            }
            else if (i == 0)
                firstMs = ms;
            else if (i >= kWarm)
                times.push_back(ms);
        }
        if (!ok)
            continue;
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        if (size.num == size.den)
            fullMs = median;

        // The Output outside the subrect, and the picture inside it.
        std::vector<uint8_t> raw;
        unsigned pitch = 0;
        unsigned long long outside = 0;
        std::vector<float> inside(size_t(sw) * sh * 3);
        if (Readback(base.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &raw, &pitch))
        {
            for (unsigned y = 0; y < h; ++y)
            {
                const auto* row = reinterpret_cast<const uint16_t*>(raw.data() + size_t(y) * pitch);
                for (unsigned x = 0; x < w; ++x)
                {
                    if (x < sw && y < sh)
                    {
                        for (unsigned c = 0; c < 3; ++c)
                            inside[(size_t(y) * sw + x) * 3 + c] = HalfToFloat(row[x * 4 + c]);
                    }
                    else if (std::memcmp(&row[x * 4], sentinel, sizeof sentinel) != 0)
                        ++outside;
                }
            }
        }
        else
        {
            Note("T9 %s: could not read the Output back", size.name);
            continue;
        }

        // The same pixels given to a feature created at the subrect's size.
        double ownMs = 0.0, diff = -1.0;
        if (size.num != size.den)
        {
            NVSDK_NGX_Parameter* ownBlock = nullptr;
            void* own = CreateOwn(m, sw, sh, nullptr, &ownBlock);
            ID3D12Resource* colour = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, sw, sh, D3D12_RESOURCE_FLAG_NONE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12Resource* output = MakeOutput(sw, sh);
            if (own != nullptr && colour != nullptr && output != nullptr)
            {
                Barrier(base.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from = {};
                from.pResource = base.colour;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION to = from;
                to.pResource = colour;
                const D3D12_BOX box = { 0, 0, 0, sw, sh, 1 };
                g.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
                Barrier(base.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(colour, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Inputs mine = in;
                mine.colour = colour;
                mine.output = output;
                std::vector<double> ownTimes;
                bool ownOk = Submit("copy") >= 0.0;
                for (unsigned i = 0; i < kWarm + kTimed && ownOk; ++i)
                {
                    SetTunables(ownBlock, defaults);
                    SetInputs(ownBlock, mine, i == 0 ? 1u : 0u);
                    const int r = m.evaluate(g.list, own, ownBlock, nullptr);
                    const double ms = Submit("evaluate");
                    ownOk = r == NVSDK_NGX_Result_Success && ms >= 0.0;
                    if (ownOk && i >= kWarm)
                        ownTimes.push_back(ms);
                }
                std::vector<float> ownPicture;
                if (ownOk && ReadRgb(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, sw, sh, &ownPicture))
                {
                    std::sort(ownTimes.begin(), ownTimes.end());
                    ownMs = ownTimes[ownTimes.size() / 2];
                    diff = Difference(inside, ownPicture);
                }
                else
                    Note("T9 %s: the feature created at %ux%u did not run", size.name, sw, sh);
            }
            else
                Note("T9 %s: no feature or textures at %ux%u to compare with", size.name, sw, sh);
            if (own != nullptr)
                m.release(own);
            if (colour != nullptr)
                colour->Release();
            if (output != nullptr)
                output->Release();
        }
        if (size.num == size.den)
            Note("T9 %s (%ux%u): median %.2f ms, the first after the change %.2f ms", size.name, sw, sh, median,
                 firstMs);
        else
            Note("T9 %s (%ux%u in %ux%u): accepted; median %.2f ms (%.0f%% of 100%%), the first after the change "
                 "%.2f ms, a feature created at this size %.2f ms; %llu pixels outside the subrect changed; picture "
                 "against that feature's: %.6f",
                 size.name, sw, sh, w, h, median, fullMs > 0.0 ? 100.0 * median / fullMs : 0.0, firstMs, ownMs,
                 outside, diff);
    }
    // The feature goes back to its whole frame for whatever runs next.
    SetInputs(evalBlock, base, 1u);
}

// T10: T5 again with the picture at two thirds of the frame the motion vectors describe, as the games' motion vectors
// will be with ModelScale at 66.7%: the motion vectors a texture at 0.58 of the frame in UV units, MVecScale scanned as
// f times the frame's width. If the model reads the motion against the motion vectors' own size, the games' f = 0.58
// stays best; against the picture's size, f = 0.67 would be; against the size the feature was created at, f = 1.
// Run both ways: inside the basic run's feature (a subrect, what dxgi.dll does) and on a feature created at that size.
void Subrects10(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& base)
{
    constexpr unsigned kFrames = 16;
    constexpr int kSpeed = 4; // pixels of the picture per frame: 6 of the frame
    constexpr float kNoise = 0.06f;
    const unsigned W = base.width, H = base.height;
    const unsigned w = (W * 2 + 1) / 3, h = (H * 2 + 1) / 3;
    const unsigned margin = w / 12;
    if (w <= 2 * margin + 64 || h <= 2 * margin + 64)
    {
        Note("T10 skipped: the frame is too small");
        return;
    }
    const unsigned mw = unsigned(std::lround(0.58 * W)), mh = unsigned(std::lround(0.58 * H));
    std::vector<uint16_t> moving(size_t(mw) * mh * 2, FloatToHalf(0.0f));
    for (size_t i = 0; i < size_t(mw) * mh; ++i)
        moving[i * 2] = FloatToHalf(-float(kSpeed) / float(w));
    ID3D12Resource* motion = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, mw, mh, moving.data(), mw * 4);
    ID3D12Resource* bigColour = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, D3D12_RESOURCE_FLAG_NONE,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* smallColour = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* smallOutput = MakeOutput(w, h);
    NVSDK_NGX_Parameter* ownBlock = nullptr;
    void* own = CreateOwn(m, w, h, nullptr, &ownBlock);
    if (motion == nullptr || bigColour == nullptr || smallColour == nullptr || smallOutput == nullptr || own == nullptr)
    {
        Note("T10 skipped: could not make its textures or its feature at %ux%u", w, h);
        for (ID3D12Resource* r : { motion, bigColour, smallColour, smallOutput })
            if (r != nullptr)
                r->Release();
        if (own != nullptr)
            m.release(own);
        return;
    }
    D3D12_RESOURCE_STATES bigState = D3D12_RESOURCE_STATE_COPY_DEST, smallState = D3D12_RESOURCE_STATE_COPY_DEST;

    const unsigned pad = kSpeed * kFrames;
    const unsigned cw = W + pad;
    std::vector<float> card(size_t(cw) * H * 3);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < cw; ++x)
            for (unsigned c = 0; c < 3; ++c)
                card[(size_t(y) * cw + x) * 3 + c] = Card(int(x) - int(pad), int(y), c);
    const Tunables defaults;

    // One run: E over the last four frames, or a negative number when something failed. `inside`: the basic run's
    // feature with a subrect; else the feature created at the picture's size.
    auto run = [&](bool inside, float f, bool resetEach) -> double {
        g_random = 0x9E3779B9u;
        const unsigned tw = inside ? W : w, th = inside ? H : h; // the textures
        ID3D12Resource* colour = inside ? bigColour : smallColour;
        D3D12_RESOURCE_STATES& state = inside ? bigState : smallState;
        Inputs in = base;
        in.width = w;
        in.height = h;
        in.colour = colour;
        in.output = inside ? base.output : smallOutput;
        in.motion = motion;
        in.motionWidth = mw;
        in.motionHeight = mh;
        in.mvScaleX = f * float(W);
        in.mvScaleY = f * float(H);
        void* feature = inside ? handle : own;
        NVSDK_NGX_Parameter* block = inside ? evalBlock : ownBlock;
        std::vector<uint16_t> frame(size_t(tw) * th * 4, FloatToHalf(0.0f));
        std::vector<float> out;
        double sum = 0.0;
        unsigned counted = 0;
        for (unsigned t = 0; t < kFrames; ++t)
        {
            const unsigned offset = pad - unsigned(kSpeed) * t;
            for (unsigned y = 0; y < h; ++y)
            {
                const float* row = &card[(size_t(y) * cw + offset) * 3];
                uint16_t* to = &frame[size_t(y) * tw * 4];
                for (unsigned x = 0; x < w; ++x)
                {
                    for (unsigned c = 0; c < 3; ++c)
                        to[x * 4 + c] = FloatToHalf(row[x * 3 + c] + kNoise * Noise());
                    to[x * 4 + 3] = FloatToHalf(1.0f);
                }
            }
            if (!UploadNow(colour, frame.data(), tw * 8, th, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                return -1.0;
            state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            SetTunables(block, defaults);
            if (!EvaluateOnce(m, feature, block, in, t == 0 || resetEach ? 1u : 0u))
                return -1.0;
            if (t + 4 < kFrames)
                continue;
            if (!ReadRgb(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, w, h, &out))
                return -1.0;
            double e = 0.0;
            size_t n = 0;
            for (unsigned y = margin; y + margin < h; ++y)
            {
                const float* clean = &card[(size_t(y) * cw + offset) * 3];
                for (unsigned x = margin; x + margin < w; ++x)
                {
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const double d = std::fabs(double(out[(size_t(y) * w + x) * 3 + c]) - double(clean[x * 3 + c]));
                        e += d == d ? d : 1.0;
                        ++n;
                    }
                }
            }
            sum += e / double(n);
            ++counted;
        }
        return counted != 0 ? sum / double(counted) : -1.0;
    };

    Say("T10 motion vectors at a smaller picture: the picture %ux%u of a %ux%u frame scrolls %d px a frame, motion "
        "vectors %ux%u in UV units, MVecScale = f x the frame's size",
        w, h, W, H, kSpeed, mw, mh);
    const float factors[] = { 0.39f, 0.5f, 0.58f, 0.67f, 0.79f, 1.0f };
    for (int way = 0; way < 2; ++way)
    {
        const bool inside = way == 0;
        const char* name = inside ? "inside the feature (subrect)" : "a feature created at the picture's size";
        const double eReset = run(inside, 0.58f, true);
        double e[6] = {};
        int best = -1;
        for (int i = 0; i < 6; ++i)
        {
            e[i] = run(inside, factors[i], false);
            if (e[i] >= 0.0 && (best < 0 || e[i] < e[best]))
                best = i;
        }
        Note("T10 %s: no history E %.5f; f 0.39 %.5f, 0.50 %.5f, 0.58 %.5f (the games'), 0.67 %.5f (the picture's "
             "share), 0.79 %.5f, 1.00 %.5f (the frame's)",
             name, eReset, e[0], e[1], e[2], e[3], e[4], e[5]);
        if (best < 0 || eReset < 0.0)
            Note("T10 %s verdict: runs failed (above)", name);
        else if (eReset - e[best] <= 1.0e-3)
            Note("T10 %s verdict: inconclusive, history barely helps (%.5f)", name, eReset - e[best]);
        else
            Note("T10 %s verdict: best f = %.2f%s", name, double(factors[best]),
                 best == 2 ? ": the games' motion vectors line up as they are" : ": NOT the games' scale, look closer");
    }
    for (ID3D12Resource* r : { motion, bigColour, smallColour, smallOutput })
        r->Release();
    m.release(own);
    SetInputs(evalBlock, base, 1u);
}

// The --tuning checks, on the feature the basic run made (`handle`, with its evaluation block `evalBlock`, which has
// never held a tunable), over the basic run's frame (`in`). Every run starts from Reset and evaluates its frame
// kFrames times; outputs are compared as the mean absolute difference per channel. "Same" is within four times
// the difference between two identical runs (at least 1e-5), "differs" at least ten times it (at least 2e-4).
//   T0  no tunable keys at all = the model's defaults written out (what dxgi.dll writes for an unset key)
//   T1  each tunable written at evaluation changes the picture
//   T2  tunables written only into the creation block change nothing (what v0.1.0 and v0.1.1 did)
//   T3  Style 3 = Style 2, Intensity 1.5 = 1, skin structure without the auto mask = nothing (informational)
//   T4  depth: another depth, DepthInverted, no depth at all (informational: the model is said to ignore it)
//   T5  motion vectors: the card scrolls, the scale is scanned; which scale the model wants (informational)
//   T6  the model's colour on real Witcher 3 frames from dxgi.dll's dumps, per setting (informational)
//   T7  motion vectors dilated by depth, against the games' own and per-pixel ones, along the edges of a moving
//       square and on moving thin bars (informational)
//   T8  the control mask: constant masks against the sliders, values above 1, how far a split mask's halves reach
//       into each other, a mask at half size (informational)
void Tuning(const Model& m, void* handle, NVSDK_NGX_Parameter* evalBlock, const Inputs& in,
            const std::wstring& dumpFolder)
{
    constexpr unsigned kFrames = 6;
    const unsigned w = in.width, h = in.height;
    Say("tuning: every run starts from Reset and evaluates its frame %u times; numbers are mean |difference| per "
        "channel",
        kFrames);

    std::vector<float> absent, base, base2;
    if (!Run(m, handle, evalBlock, in, nullptr, kFrames, &absent))
    {
        Expect(false, "T0 the run without tunable keys");
        return;
    }
    const Tunables defaults;
    if (!Run(m, handle, evalBlock, in, &defaults, kFrames, &base) ||
        !Run(m, handle, evalBlock, in, &defaults, kFrames, &base2))
    {
        Expect(false, "T0 the runs with the defaults written");
        return;
    }
    const double noise = Difference(base, base2);
    const double same = noise * 4.0 > 1.0e-5 ? noise * 4.0 : 1.0e-5;
    const double differs = noise * 10.0 > 2.0e-4 ? noise * 10.0 : 2.0e-4;
    Say("T0 two identical runs differ by %.6f: same <= %.6f, differs >= %.6f", noise, same, differs);
    const double absentVsBase = Difference(absent, base);
    Expect(absentVsBase <= same, "T0 no tunable keys = the model's defaults written out (%.6f)", absentVsBase);

    struct Case
    {
        const char* name;
        Tunables t;
    };
    Tunables t;
    std::vector<Case> cases;
    t = defaults;
    t.style = 1;
    cases.push_back({ "Style 1 (natural)", t });
    t = defaults;
    t.style = 2;
    cases.push_back({ "Style 2 (cinematic)", t });
    t = defaults;
    t.localTone = 0.25f;
    cases.push_back({ "LocalTone 0.25", t });
    t = defaults;
    t.localStructure = 0.25f;
    cases.push_back({ "LocalStructure 0.25", t });
    t = defaults;
    t.intensity = 0.5f;
    cases.push_back({ "Intensity 0.5", t });
    t = defaults;
    t.autoMask = 1;
    cases.push_back({ "AutoMask 1", t });
    std::vector<double> moved;
    for (const Case& c : cases)
    {
        std::vector<float> out;
        if (!Run(m, handle, evalBlock, in, &c.t, kFrames, &out))
        {
            Expect(false, "T1 %s: the run failed", c.name);
            moved.push_back(0.0);
            continue;
        }
        moved.push_back(Difference(out, base));
    }
    Expect(moved[4] >= differs, "T1 Intensity 0.5 written at evaluation changes the picture (%.6f)", moved[4]);
    Expect(moved[0] >= differs || moved[1] >= differs || moved[2] >= differs || moved[3] >= differs,
           "T1 Style or a local strength written at evaluation changes the picture (style 1 %.6f, style 2 %.6f, "
           "local tone %.6f, local structure %.6f)",
           moved[0], moved[1], moved[2], moved[3]);
    for (size_t i = 0; i < cases.size(); ++i)
        Note("T1 %-22s %.6f %s", cases[i].name, moved[i],
             moved[i] >= differs ? "changes the picture" : moved[i] <= same ? "no change" : "a small change");

    // T2: the values only in the creation block of a second feature, its evaluation block never given any.
    Tunables atCreate = defaults;
    atCreate.intensity = 0.5f;
    atCreate.style = 2;
    atCreate.localTone = 0.25f;
    atCreate.localStructure = 0.25f;
    atCreate.autoMask = 1;
    std::vector<float> perFrame, createOnly;
    NVSDK_NGX_Parameter* evalBlock2 = nullptr;
    if (!Run(m, handle, evalBlock, in, &atCreate, kFrames, &perFrame))
        Expect(false, "T2 the run with the values written at evaluation");
    else if (m.allocate == nullptr)
        Note("T2 skipped: --capability has no parameter blocks of our own for a second feature");
    else if (void* second = CreateOwn(m, w, h, &atCreate, &evalBlock2); second == nullptr)
        Expect(false, "T2 a second feature with the values in its creation block");
    else
    {
        const bool ran = Run(m, second, evalBlock2, in, nullptr, kFrames, &createOnly);
        const int released = m.release(second);
        if (!ran)
            Expect(false, "T2 the second feature's run");
        else
        {
            const double vsAbsent = Difference(createOnly, absent);
            const double vsPerFrame = Difference(createOnly, perFrame);
            const double perFrameVsAbsent = Difference(perFrame, absent);
            Expect(vsAbsent <= same,
                   "T2 values only in the creation block change nothing: the same as no keys (%.6f; the same values "
                   "at evaluation move it %.6f)",
                   vsAbsent, perFrameVsAbsent);
            Note("T2 creation-only vs the same values at evaluation: %.6f; ReleaseFeature -> 0x%08X", vsPerFrame,
                 unsigned(released));
        }
    }

    // T3: the clamps and the mask, as the model's code reads them.
    struct Pair
    {
        const char* what;
        Tunables a, b;
    };
    std::vector<Pair> pairs;
    Tunables a = defaults, b = defaults;
    a.style = 3;
    b.style = 2;
    pairs.push_back({ "Style 3 vs Style 2", a, b });
    a = defaults;
    a.intensity = 1.5f;
    pairs.push_back({ "Intensity 1.5 vs 1", a, defaults });
    a = defaults;
    a.skin = 0.0f;
    pairs.push_back({ "SkinStructure 0, auto mask off, vs defaults", a, defaults });
    a.skin = 1.5f;
    pairs.push_back({ "SkinStructure 1.5, auto mask off, vs defaults", a, defaults });
    a = defaults;
    b = defaults;
    a.autoMask = b.autoMask = 1;
    a.skin = 0.0f;
    pairs.push_back({ "SkinStructure 0 vs -1, auto mask on (no skin in this frame)", a, b });
    for (const Pair& pair : pairs)
    {
        std::vector<float> oa, ob;
        if (!Run(m, handle, evalBlock, in, &pair.a, kFrames, &oa) || !Run(m, handle, evalBlock, in, &pair.b, kFrames, &ob))
        {
            Note("T3 %s: a run failed", pair.what);
            continue;
        }
        const double d = Difference(oa, ob);
        Note("T3 %-58s %.6f %s", pair.what, d, d <= same ? "the same" : d >= differs ? "differs" : "close");
    }

    // T4: depth.
    {
        std::vector<float> flat(size_t(w) * h, 0.75f);
        ID3D12Resource* otherDepth = MakeFilled(DXGI_FORMAT_R32_FLOAT, w, h, flat.data(), w * 4);
        std::vector<float> out;
        Inputs changed = in;
        if (otherDepth != nullptr)
        {
            changed.depth = otherDepth;
            if (Run(m, handle, evalBlock, changed, &defaults, kFrames, &out))
                Note("T4 a flat depth instead of the ramp: %.6f", Difference(out, base));
        }
        changed = in;
        changed.depthInverted = 1;
        if (Run(m, handle, evalBlock, changed, &defaults, kFrames, &out))
            Note("T4 DepthInverted 1: %.6f", Difference(out, base));
        changed = in;
        changed.depth = nullptr;
        if (Run(m, handle, evalBlock, changed, &defaults, kFrames, &out))
            Note("T4 no depth at all: accepted, %.6f", Difference(out, base));
        else
            Note("T4 no depth at all: refused (above)");
        if (otherDepth != nullptr)
            otherDepth->Release();
    }

    Tuning5(m, handle, evalBlock, in);
    Tuning6(m, dumpFolder);
    Tuning7(m, handle, evalBlock, in);
    Tuning8(m, handle, evalBlock, in, base, same, differs);
}

// ---------------------------------------------------------------------------------------------------------------
// --captures: the Witcher 3 colour drift, on the files of the capture build of dxgi.dll (a branch of its own, never
// released). Every 30 s it writes three files side by side into a folder named debug on the desktop:
// <game>-<date>-<time>.bzdump (the frame dump), .bzframe (the whole frame as DLSS made it, and what the model was
// given besides its picture) and .bmp (the finished picture the game presented; the composite is skipped around the
// capture, so it is the game's own). Per capture: whether the three agree; how the game's picture differs from our
// proxy; how much bluer (+) or warmer (-) the model makes the picture given our proxy (as in the game), the game's own
// picture, our proxy with the game's tone curve, and our proxy at other exposures and with other settings; and what a
// colour lock on our side would leave of it. Then the same over all the captures: the drift is how differently the
// model colours one view and the next, so what tells is the range of its change across them.

constexpr uint32_t kDumpLinearHdr = 0x01; // the dump's flags: nr_shared.h, NR_FLAG_LINEAR_HDR
constexpr unsigned kRgba16f = 10;         // DXGI_FORMAT_R16G16B16A16_FLOAT
constexpr unsigned kR11G11B10 = 26;       // DXGI_FORMAT_R11G11B10_FLOAT
constexpr unsigned kReplayFrames = 12;    // evaluations of the same picture from a reset: the model's history settles
constexpr unsigned kShiftStep = 2;        // full-size pictures are measured on every other pixel of every other row
constexpr double kAgreementNeeded = 0.9;  // the finished picture's correlation with the frame (Agreement)

// A picture as RGB floats, row by row: linear light for a frame, display-encoded for the model's input and output.
struct Image
{
    unsigned width = 0, height = 0;
    std::vector<float> rgb;

    size_t Pixels() const { return size_t(width) * height; }
};

// The model run on a picture of our own with these parameters and, when `mask` is given, a control mask of the
// picture's size (per pixel: the final blend, LocalTone and LocalStructure, as T8 has it), its output read back; false
// when it failed (said why).
using Replay = std::function<bool(const Image& input, const Tunables& t, const Image* mask, Image* output)>;

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* data)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return false;
    bool ok = _fseeki64(f, 0, SEEK_END) == 0;
    const long long size = ok ? _ftelli64(f) : -1;
    ok = size > 0 && _fseeki64(f, 0, SEEK_SET) == 0;
    if (ok)
    {
        data->resize(size_t(size));
        ok = std::fread(data->data(), 1, data->size(), f) == data->size();
    }
    std::fclose(f);
    return ok;
}

// A frame value the model can be given (nr_common.hlsli, Clean): NaN and below zero are 0, infinity a large number.
float Clean1(float v) { return std::isnan(v) ? 0.0f : std::min(std::max(v, 0.0f), 1.0e30f); }

// sRGB, both ways, held to 0..1 (NaN is 0).
float Encode(float v)
{
    if (!(v > 0.0f))
        return 0.0f;
    if (v >= 1.0f)
        return 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float Decode(float e)
{
    if (!(e > 0.0f))
        return 0.0f;
    if (e >= 1.0f)
        return 1.0f;
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float Max3(const float* c) { return std::max(c[0], std::max(c[1], c[2])); }
float Saturation(const float* c)
{
    const float most = Max3(c);
    return most > 1.0e-6f ? (most - std::min(c[0], std::min(c[1], c[2]))) / most : 0.0f;
}

// One channel of DXGI_FORMAT_R11G11B10_FLOAT: 5 bits of exponent, `mantissaBits` of mantissa, no sign.
float SmallFloat(uint32_t bits, unsigned mantissaBits)
{
    const uint32_t exponent = bits >> mantissaBits;
    const float fraction = float(bits & ((1u << mantissaBits) - 1u)) / float(1u << mantissaBits);
    if (exponent == 0)
        return std::ldexp(fraction, -14);
    if (exponent == 31)
        return fraction == 0.0f ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    return std::ldexp(1.0f + fraction, int(exponent) - 15);
}

const char* FormatName(unsigned format)
{
    switch (format)
    {
    case 0:
        return "none";
    case 10:
        return "RGBA16F";
    case 24:
        return "R10G10B10A2";
    case 26:
        return "R11G11B10F";
    case 28:
        return "RGBA8";
    case 29:
        return "RGBA8 sRGB";
    case 87:
        return "BGRA8";
    case 88:
        return "BGRX8";
    case 91:
        return "BGRA8 sRGB";
    default:
        return "another format";
    }
}

// The capture build's .bzframe (its nr_dx12.cpp, FrameFileHeader): 168 bytes of header, then the frame's rows.
struct FrameFile
{
    uint64_t number = 0; // our frame counter, the .bzdump's
    unsigned format = 0;
    unsigned modelWidth = 0, modelHeight = 0;
    Tunables t; // the model's six parameters at the capture
    bool mask = false, dilate = false;
    unsigned pictureFormat = 0; // the back buffer's DXGI_FORMAT; 0: no finished picture
    unsigned pictureWidth = 0, pictureHeight = 0;
    bool displayHdr = false;
    Image frame; // as DLSS made it, before the composite: linear light for a linear HDR Output
};

bool ReadFrameFile(const std::wstring& path, FrameFile* f)
{
    std::vector<uint8_t> data;
    if (!ReadWhole(path, &data) || data.size() < 168 || std::memcmp(data.data(), "BZFRAME1", 8) != 0)
        return false;
    auto word = [&](size_t at)
    {
        uint32_t v = 0;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    };
    auto real = [&](size_t at)
    {
        float v = 0.0f;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    };
    std::memcpy(&f->number, data.data() + 8, 8);
    const uint32_t headerBytes = word(16), width = word(24), height = word(28), rowBytes = word(32);
    f->format = word(20);
    f->modelWidth = word(36);
    f->modelHeight = word(40);
    f->t.intensity = real(56);
    f->t.style = word(60);
    f->t.localStructure = real(64);
    f->t.localTone = real(68);
    f->t.skin = real(72);
    f->t.autoMask = int(word(76));
    f->mask = word(80) != 0;
    f->dilate = word(84) != 0;
    f->pictureFormat = word(88);
    f->pictureWidth = word(92);
    f->pictureHeight = word(96);
    f->displayHdr = word(100) != 0;
    const unsigned pixelBytes = f->format == kRgba16f ? 8u : f->format == kR11G11B10 ? 4u : 0u;
    if (headerBytes < 168 || width == 0 || height == 0 || pixelBytes == 0 || rowBytes < uint64_t(width) * pixelBytes ||
        uint64_t(headerBytes) + uint64_t(rowBytes) * height > data.size())
        return false;
    Image& im = f->frame;
    im.width = width;
    im.height = height;
    im.rgb.resize(im.Pixels() * 3);
    for (unsigned y = 0; y < height; ++y)
    {
        const uint8_t* row = data.data() + headerBytes + size_t(y) * rowBytes;
        float* to = im.rgb.data() + size_t(y) * width * 3;
        for (unsigned x = 0; x < width; ++x)
        {
            if (pixelBytes == 8)
            {
                uint16_t half[3] = {};
                std::memcpy(half, row + size_t(x) * 8, 6);
                for (unsigned c = 0; c < 3; ++c)
                    to[x * 3 + c] = HalfToFloat(half[c]);
            }
            else
            {
                uint32_t v = 0;
                std::memcpy(&v, row + size_t(x) * 4, 4);
                to[x * 3] = SmallFloat(v & 0x7FFu, 6);
                to[x * 3 + 1] = SmallFloat((v >> 11) & 0x7FFu, 6);
                to[x * 3 + 2] = SmallFloat(v >> 22, 5);
            }
        }
    }
    return true;
}

// The capture build's finished picture (its nr_dx12.cpp, WriteBmp): 24-bit, rows padded to four bytes, as
// display-encoded RGB 0..1.
bool ReadBmp(const std::wstring& path, Image* out)
{
    std::vector<uint8_t> data;
    if (!ReadWhole(path, &data) || data.size() < 54 || data[0] != 'B' || data[1] != 'M')
        return false;
    uint32_t pixelsAt = 0, compression = 0;
    int32_t width = 0, height = 0;
    uint16_t bits = 0;
    std::memcpy(&pixelsAt, data.data() + 10, 4);
    std::memcpy(&width, data.data() + 18, 4);
    std::memcpy(&height, data.data() + 22, 4);
    std::memcpy(&bits, data.data() + 28, 2);
    std::memcpy(&compression, data.data() + 30, 4);
    if (bits != 24 || compression != 0 || width <= 0 || height == 0 || height == INT32_MIN)
        return false;
    const unsigned w = unsigned(width), h = unsigned(height < 0 ? -height : height);
    const size_t rowBytes = (size_t(w) * 3 + 3) & ~size_t(3);
    if (uint64_t(pixelsAt) + uint64_t(rowBytes) * h > data.size())
        return false;
    out->width = w;
    out->height = h;
    out->rgb.resize(out->Pixels() * 3);
    for (unsigned y = 0; y < h; ++y)
    {
        const unsigned fileRow = height > 0 ? h - 1 - y : y; // positive height: the bottom row first
        const uint8_t* from = data.data() + pixelsAt + size_t(fileRow) * rowBytes;
        float* to = out->rgb.data() + size_t(y) * w * 3;
        for (unsigned x = 0; x < w; ++x)
        {
            to[x * 3] = float(from[x * 3 + 2]) / 255.0f;
            to[x * 3 + 1] = float(from[x * 3 + 1]) / 255.0f;
            to[x * 3 + 2] = float(from[x * 3]) / 255.0f;
        }
    }
    return true;
}

// Our proxy, the model's input, from the frame: for a linear HDR frame the white point, then the shoulder on the
// largest channel, then sRGB (nr_common.hlsli, EncodeLinear; nr.hlsl, the encode); a display-encoded frame as it is.
Image EncodeProxy(const Image& frame, bool linear, float white, float shoulder)
{
    Image p;
    p.width = frame.width;
    p.height = frame.height;
    p.rgb.resize(frame.rgb.size());
    for (size_t i = 0; i < frame.rgb.size(); i += 3)
    {
        float x[3] = {};
        for (unsigned c = 0; c < 3; ++c)
            x[c] = Clean1(frame.rgb[i + c]) / (linear ? white : 1.0f);
        if (!linear)
        {
            for (unsigned c = 0; c < 3; ++c)
                p.rgb[i + c] = std::min(x[c], 1.0f);
            continue;
        }
        const float m = Max3(x);
        float k = 1.0f;
        if (m > shoulder && shoulder < 1.0f)
        {
            const float t = (m - shoulder) / (1.0f - shoulder);
            k = (shoulder + (1.0f - shoulder) * (t / (1.0f + t))) / m;
        }
        for (unsigned c = 0; c < 3; ++c)
            p.rgb[i + c] = Encode(std::min(x[c] * k, 1.0f));
    }
    return p;
}

// The mean of each `factor` square, as the dump reduces the frame (nr.hlsl, DumpRead): the right and bottom edges
// that make no whole square are left out.
Image Reduce(const Image& a, unsigned factor)
{
    Image r;
    r.width = a.width / factor;
    r.height = a.height / factor;
    r.rgb.assign(r.Pixels() * 3, 0.0f);
    const float scale = 1.0f / float(factor * factor);
    for (unsigned y = 0; y < r.height; ++y)
    {
        for (unsigned x = 0; x < r.width; ++x)
        {
            for (unsigned c = 0; c < 3; ++c)
            {
                float sum = 0.0f;
                for (unsigned dy = 0; dy < factor; ++dy)
                    for (unsigned dx = 0; dx < factor; ++dx)
                        sum += a.rgb[((size_t(y) * factor + dy) * a.width + size_t(x) * factor + dx) * 3 + c];
                r.rgb[(size_t(y) * r.width + x) * 3 + c] = sum * scale;
            }
        }
    }
    return r;
}

// A display-encoded picture at another size, made the way the pass makes the model's smaller picture with ModelScale
// (nr.hlsl, EncodeScaled): each new pixel the mean, in linear light, of the old pixels it covers, each weighted by how
// much of it it covers.
Image Resize(const Image& a, unsigned width, unsigned height)
{
    struct Tap
    {
        unsigned at;
        float weight;
    };
    auto taps = [](unsigned from, unsigned to)
    {
        std::vector<std::vector<Tap>> list(to);
        const double step = double(from) / double(to);
        for (unsigned q = 0; q < to; ++q)
        {
            const double lo = double(q) * step, hi = std::min(lo + step, double(from));
            const unsigned end = std::min(unsigned(std::ceil(hi)), from);
            for (unsigned i = unsigned(lo); i < end; ++i)
            {
                const double w = std::min(hi, double(i) + 1.0) - std::max(lo, double(i));
                if (w > 0.0)
                    list[q].push_back({ i, float(w) });
            }
        }
        return list;
    };
    const std::vector<std::vector<Tap>> across = taps(a.width, width), down = taps(a.height, height);
    std::vector<float> light(a.rgb.size());
    for (size_t i = 0; i < a.rgb.size(); ++i)
        light[i] = Decode(a.rgb[i]);
    Image r;
    r.width = width;
    r.height = height;
    r.rgb.resize(r.Pixels() * 3);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            float sum[3] = {};
            float weight = 0.0f;
            for (const Tap& ty : down[y])
            {
                for (const Tap& tx : across[x])
                {
                    const float w = ty.weight * tx.weight;
                    const float* c = light.data() + (size_t(ty.at) * a.width + tx.at) * 3;
                    sum[0] += w * c[0];
                    sum[1] += w * c[1];
                    sum[2] += w * c[2];
                    weight += w;
                }
            }
            for (unsigned c = 0; c < 3; ++c)
                r.rgb[(size_t(y) * width + x) * 3 + c] = Encode(weight > 0.0f ? sum[c] / weight : 0.0f);
        }
    }
    return r;
}

// A picture at the model's size seen at the frame's pixels, as the dump reads the model's input and output with
// ModelScale (nr.hlsl, ModelTexel): each frame pixel takes the model texel it falls in.
Image AtFramePixels(const Image& a, unsigned width, unsigned height)
{
    const float stepX = float(width) / float(a.width), stepY = float(height) / float(a.height);
    Image r;
    r.width = width;
    r.height = height;
    r.rgb.resize(r.Pixels() * 3);
    for (unsigned y = 0; y < height; ++y)
    {
        const unsigned ty = std::min(unsigned((float(y) + 0.5f) / stepY), a.height - 1);
        for (unsigned x = 0; x < width; ++x)
        {
            const unsigned tx = std::min(unsigned((float(x) + 0.5f) / stepX), a.width - 1);
            for (unsigned c = 0; c < 3; ++c)
                r.rgb[(size_t(y) * width + x) * 3 + c] = a.rgb[(size_t(ty) * a.width + tx) * 3 + c];
        }
    }
    return r;
}

// Rows y0..y1 of a picture.
Image Rows(const Image& a, unsigned y0, unsigned y1)
{
    Image r;
    r.width = a.width;
    r.height = y1 - y0;
    r.rgb.assign(a.rgb.begin() + std::ptrdiff_t(size_t(y0) * a.width * 3),
                 a.rgb.begin() + std::ptrdiff_t(size_t(y1) * a.width * 3));
    return r;
}

// How closely the finished picture follows the frame: the correlation, over squares of 16 pixels, of the log2 of
// their mean luminance (the frame's linear, the picture's decoded). The game's curve rises with the frame's light, so
// the same view gives close to 1; a moved camera, a menu or another view does not. NaN when there is too little.
double Agreement(const Image& frame, const Image& picture)
{
    constexpr unsigned kSquare = 16;
    std::vector<double> a, b;
    for (unsigned by = 0; by + kSquare <= frame.height; by += kSquare)
    {
        for (unsigned bx = 0; bx + kSquare <= frame.width; bx += kSquare)
        {
            double sumFrame = 0.0, sumPicture = 0.0;
            for (unsigned y = by; y < by + kSquare; ++y)
            {
                for (unsigned x = bx; x < bx + kSquare; ++x)
                {
                    const size_t i = (size_t(y) * frame.width + x) * 3;
                    const float f[3] = { Clean1(frame.rgb[i]), Clean1(frame.rgb[i + 1]), Clean1(frame.rgb[i + 2]) };
                    const float p[3] = { Decode(picture.rgb[i]), Decode(picture.rgb[i + 1]),
                                         Decode(picture.rgb[i + 2]) };
                    sumFrame += Luma(f);
                    sumPicture += Luma(p);
                }
            }
            const double n = double(kSquare * kSquare);
            a.push_back(std::log2(sumFrame / n + 1.0e-6));
            b.push_back(std::log2(sumPicture / n + 1.0e-6));
        }
    }
    if (a.size() < 16)
        return std::numeric_limits<double>::quiet_NaN();
    double meanA = 0.0, meanB = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= double(a.size());
    meanB /= double(b.size());
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        ab += (a[i] - meanA) * (b[i] - meanB);
        aa += (a[i] - meanA) * (a[i] - meanA);
        bb += (b[i] - meanB) * (b[i] - meanB);
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : std::numeric_limits<double>::quiet_NaN();
}

// Whether a display-linear pixel is bright and blue, as a clear sky is: luminance above 0.18, blue above red by 15%.
bool SkyLikePixel(const float* c) { return Luma(c) > 0.18f && c[2] > 1.15f * c[0]; }

// The share of a display-encoded picture that is sky-like: which captures look at the sky.
double SkyLike(const Image& p)
{
    unsigned long long count = 0, sky = 0;
    for (unsigned y = 0; y < p.height; y += kShiftStep)
    {
        for (unsigned x = 0; x < p.width; x += kShiftStep)
        {
            const size_t i = (size_t(y) * p.width + x) * 3;
            const float c[3] = { Decode(p.rgb[i]), Decode(p.rgb[i + 1]), Decode(p.rgb[i + 2]) };
            ++count;
            if (SkyLikePixel(c))
                ++sky;
        }
    }
    return count != 0 ? double(sky) / double(count) : 0.0;
}

// The game's finished picture against our proxy, by the frame's light: per band of EV from the white point, the
// share of the frame, the mean luminance of each (display-linear), their mean saturation ((most - least) / most), and
// how much bluer the game's picture is (the mean of log2(blue / red), the picture's less the proxy's, where neither is
// near black or clipped).
void ToneTable(const char* tag, const Image& frame, float white, const Image& ours, const Image& picture)
{
    static const char* const kNames[] = { "below -6", "-6 to -4", "-4 to -2", "-2 to -1",
                                          "-1 to 0",  "0 to +1",  "above +1" };
    static const double kUpper[] = { -6.0, -4.0, -2.0, -1.0, 0.0, 1.0, 1.0e9 };
    constexpr unsigned kBands = 7;
    double count[kBands] = {}, lumOurs[kBands] = {}, lumGame[kBands] = {}, satOurs[kBands] = {}, satGame[kBands] = {};
    double cast[kBands] = {}, castCount[kBands] = {};
    double total = 0.0;
    for (unsigned y = 0; y < frame.height; y += kShiftStep)
    {
        for (unsigned x = 0; x < frame.width; x += kShiftStep)
        {
            const size_t i = (size_t(y) * frame.width + x) * 3;
            float f[3] = {}, p[3] = {}, s[3] = {};
            for (unsigned c = 0; c < 3; ++c)
            {
                f[c] = Clean1(frame.rgb[i + c]);
                p[c] = Decode(ours.rgb[i + c]);
                s[c] = Decode(picture.rgb[i + c]);
            }
            const float light = Luma(f) / white;
            const double ev = light > 0.0f ? std::log2(double(light)) : -1.0e9;
            unsigned b = 0;
            while (b + 1 < kBands && ev >= kUpper[b])
                ++b;
            count[b] += 1.0;
            total += 1.0;
            lumOurs[b] += Luma(p);
            lumGame[b] += Luma(s);
            satOurs[b] += Saturation(p);
            satGame[b] += Saturation(s);
            if (Luma(p) > 0.01f && Luma(s) > 0.01f && Max3(p) < 0.97f && Max3(s) < 0.97f)
            {
                cast[b] += std::log2((double(s[2]) + 1.0e-3) / (double(s[0]) + 1.0e-3)) -
                           std::log2((double(p[2]) + 1.0e-3) / (double(p[0]) + 1.0e-3));
                castCount[b] += 1.0;
            }
        }
    }
    Note("%s   the game's picture against our proxy, by the frame's light in EV from the white point:", tag);
    Note("%s     %-9s %6s %9s %9s %8s %8s %11s", tag, "EV", "share", "lum ours", "lum game", "sat ours", "sat game",
         "game bluer");
    for (unsigned b = 0; b < kBands; ++b)
    {
        if (count[b] == 0.0)
            continue;
        Note("%s     %-9s %5.1f%% %9.4f %9.4f %8.3f %8.3f %+11.3f", tag, kNames[b], 100.0 * count[b] / total,
             lumOurs[b] / count[b], lumGame[b] / count[b], satOurs[b] / count[b], satGame[b] / count[b],
             castCount[b] > 0.0 ? cast[b] / castCount[b] : 0.0);
    }
}

// The game's tone curve, read off one capture: in bins of a quarter EV of the frame's value over the white point, the
// median of the finished picture's display-linear value, for one channel or the three pooled. Every other pixel of
// every other row; a bin with fewer than 64 values takes its neighbours' line; then made to rise. The HUD and what
// moved between the frame and the picture fall out in the medians.
constexpr int kCurveBins = 80;
constexpr double kCurveLow = -14.0; // log2 of the frame's value over the white point at the first bin's low edge
constexpr double kCurveStep = 0.25;

struct Curve
{
    float y[kCurveBins] = {}; // the picture's display-linear value at each bin's centre
    bool ok = false;
};

Curve FitCurve(const Image& frame, float white, const Image& picture, unsigned channel /* 3: pooled */)
{
    std::vector<std::vector<float>> bins(kCurveBins);
    for (unsigned y = 0; y < frame.height; y += 2)
    {
        for (unsigned x = 0; x < frame.width; x += 2)
        {
            const size_t i = (size_t(y) * frame.width + x) * 3;
            for (unsigned c = 0; c < 3; ++c)
            {
                if (channel < 3 && c != channel)
                    continue;
                const float v = Clean1(frame.rgb[i + c]) / white;
                if (!(v > 0.0f))
                    continue;
                const double at = (std::log2(double(v)) - kCurveLow) / kCurveStep;
                if (at < 0.0 || at >= double(kCurveBins))
                    continue;
                bins[size_t(at)].push_back(Decode(picture.rgb[i + c]));
            }
        }
    }
    Curve k;
    bool filled[kCurveBins] = {};
    int first = -1, last = -1, count = 0;
    for (int b = 0; b < kCurveBins; ++b)
    {
        std::vector<float>& v = bins[size_t(b)];
        if (v.size() < 64)
            continue;
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        k.y[b] = v[v.size() / 2];
        filled[b] = true;
        if (first < 0)
            first = b;
        last = b;
        ++count;
    }
    if (count < 8)
        return k;
    for (int b = 0; b < kCurveBins; ++b)
    {
        if (filled[b])
            continue;
        if (b < first)
            k.y[b] = k.y[first] * float(std::exp2(double(b - first) * kCurveStep)); // in proportion to the value
        else if (b > last)
            k.y[b] = k.y[last];
        else
        {
            int lo = b - 1, hi = b + 1;
            while (!filled[lo])
                --lo;
            while (!filled[hi])
                ++hi;
            k.y[b] = k.y[lo] + float(b - lo) / float(hi - lo) * (k.y[hi] - k.y[lo]);
        }
    }
    for (int b = 1; b < kCurveBins; ++b)
        k.y[b] = std::max(k.y[b], k.y[b - 1]);
    k.ok = true;
    return k;
}

// The curve at a frame value over the white point: a straight line between bin centres, in proportion to the value
// below the first centre, held above the last.
float ApplyCurve(const Curve& k, float v)
{
    if (!(v > 0.0f))
        return 0.0f;
    const double at = (std::log2(double(v)) - kCurveLow) / kCurveStep - 0.5;
    if (at <= 0.0)
        return float(double(k.y[0]) * double(v) / std::exp2(kCurveLow + 0.5 * kCurveStep));
    if (at >= double(kCurveBins - 1))
        return k.y[kCurveBins - 1];
    const int b = int(at);
    const float f = float(at - double(b));
    return k.y[b] + f * (k.y[b + 1] - k.y[b]);
}

// Our proxy with the game's curves in place of our white point and shoulder: each channel of the frame over the white
// point through its curve (`curves`: three), then sRGB.
Image EncodeWithCurves(const Image& frame, float white, const Curve* curves)
{
    Image p;
    p.width = frame.width;
    p.height = frame.height;
    p.rgb.resize(frame.rgb.size());
    for (size_t i = 0; i < frame.rgb.size(); i += 3)
        for (unsigned c = 0; c < 3; ++c)
            p.rgb[i + c] = Encode(ApplyCurve(curves[c], Clean1(frame.rgb[i + c]) / white));
    return p;
}

// Our curve on a grey at `ev` from the white point, display-linear: the value itself up to the shoulder, then the
// shoulder's (nr_common.hlsli, Shoulder).
float OurCurve(double ev, float shoulder)
{
    const float m = float(std::exp2(ev));
    if (m <= shoulder || shoulder >= 1.0f)
        return std::min(m, 1.0f);
    const float t = (m - shoulder) / (1.0f - shoulder);
    return shoulder + (1.0f - shoulder) * (t / (1.0f + t));
}

// The curves at a few points, against ours.
void PrintCurves(const char* tag, const Curve& pooled, const Curve* each, float shoulder)
{
    static const double kPoints[] = { -8.0, -6.0, -4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0 };
    char line[4][256] = {};
    const char* const names[4] = { "ours", "the game's", "red", "blue" };
    for (unsigned r = 0; r < 4; ++r)
    {
        int at = std::snprintf(line[r], sizeof line[r], "%-10s", names[r]);
        for (double ev : kPoints)
        {
            const float v = float(std::exp2(ev));
            const float value = r == 0   ? OurCurve(ev, shoulder)
                                : r == 1 ? ApplyCurve(pooled, v)
                                : r == 2 ? ApplyCurve(each[0], v)
                                         : ApplyCurve(each[2], v);
            if (at >= 0 && size_t(at) < sizeof line[r])
                at += std::snprintf(line[r] + at, sizeof line[r] - size_t(at), " %7.4f", double(value));
        }
    }
    Note("%s   the curves, display-linear, at EV -8 -6 -4 -3 -2 -1 0 +1 +2 from the white point:", tag);
    for (unsigned r = 0; r < 4; ++r)
        if (r < 2 || (each[0].ok && each[2].ok))
            Note("%s     %s", tag, line[r]);
}

// A box `r` pixels each way over `channels` interleaved values per pixel, along the rows and then the columns; outside
// the picture counts as zero.
void BoxBlur(std::vector<float>* data, unsigned w, unsigned h, unsigned channels, unsigned r)
{
    std::vector<float>& d = *data;
    std::vector<double> line;
    auto pass = [&](unsigned lines, unsigned length, size_t lineStride, size_t stepStride)
    {
        for (unsigned l = 0; l < lines; ++l)
        {
            for (unsigned c = 0; c < channels; ++c)
            {
                const size_t start = size_t(l) * lineStride + c;
                auto at = [&](unsigned i) -> float& { return d[start + size_t(i) * stepStride]; };
                line.assign(length, 0.0);
                double sum = 0.0;
                for (unsigned i = 0; i < std::min(r, length); ++i)
                    sum += at(i);
                for (unsigned i = 0; i < length; ++i)
                {
                    if (i + r < length)
                        sum += at(i + r);
                    line[i] = sum;
                    if (i >= r)
                        sum -= at(i - r);
                }
                for (unsigned i = 0; i < length; ++i)
                    at(i) = float(line[i]);
            }
        }
    };
    pass(h, w, size_t(w) * channels, channels); // along each row
    pass(w, h, channels, size_t(w) * channels); // along each column
}

// How a colour lock takes the model's colour change: as the composite does, the output's chromaticity less the
// input's, added (nr_common.hlsli, ModelChroma); as a gain per channel, the change of log2 of each channel over the
// luminance, which is how a white balance moves a picture; or the whole of it, which is Colour 0.
enum LockKind
{
    kLockAdded,
    kLockGains,
    kLockAll,
};

// What a colour lock on our side would leave of the model's colour change on `p` (its input) and `o` (its output),
// both display-encoded: the change, taken as `kind` says, less its low-pass, and the output brought back to its own
// luminance. `radius` 0 takes the change's mean over the frame; else a box that many pixels each way, three times
// over (close to a Gaussian), worked at an eighth of the size. The shift that is left, and in `kept` the share of the
// change's energy left.
Shift ColourLock(const Image& p, const Image& o, unsigned radius, LockKind kind, double* kept)
{
    constexpr float kFloor = 1.0f / 16384.0f; // nr_common.hlsli, kChromaFloor
    constexpr float kEps = 1.0e-3f;           // as ColourShift's
    constexpr unsigned kCoarse = 8;
    const bool all = kind == kLockAll;
    const unsigned w = p.width, h = p.height;
    const size_t n = p.Pixels();
    std::vector<float> pl(n * 3), ol(n * 3), change(n * 3, 0.0f);
    std::vector<uint8_t> valid(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        for (unsigned c = 0; c < 3; ++c)
        {
            pl[i * 3 + c] = Decode(p.rgb[i * 3 + c]);
            ol[i * 3 + c] = Decode(o.rgb[i * 3 + c]);
        }
        const float yp = Luma(&pl[i * 3]), yo = Luma(&ol[i * 3]);
        if (yp > kFloor && yo > kFloor)
        {
            valid[i] = 1;
            for (unsigned c = 0; c < 3; ++c)
                change[i * 3 + c] = kind == kLockGains ? std::log2((ol[i * 3 + c] + kEps) / (pl[i * 3 + c] + kEps)) -
                                                             std::log2((yo + kEps) / (yp + kEps))
                                                       : ol[i * 3 + c] / yo - pl[i * 3 + c] / yp;
        }
    }
    // The low-pass: the mean, or the change blurred at an eighth of the size and brought back bilinearly.
    float mean[3] = {};
    std::vector<float> coarse;
    const unsigned cw = (w + kCoarse - 1) / kCoarse, ch = (h + kCoarse - 1) / kCoarse;
    if (!all && radius == 0)
    {
        double sum[3] = {}, count = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            if (valid[i] == 0)
                continue;
            for (unsigned c = 0; c < 3; ++c)
                sum[c] += change[i * 3 + c];
            count += 1.0;
        }
        for (unsigned c = 0; c < 3; ++c)
            mean[c] = count > 0.0 ? float(sum[c] / count) : 0.0f;
    }
    else if (!all)
    {
        std::vector<float> sums(size_t(cw) * ch * 3, 0.0f), weights(size_t(cw) * ch, 0.0f);
        for (unsigned y = 0; y < h; ++y)
        {
            for (unsigned x = 0; x < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                if (valid[i] == 0)
                    continue;
                const size_t cell = size_t(y / kCoarse) * cw + x / kCoarse;
                for (unsigned c = 0; c < 3; ++c)
                    sums[cell * 3 + c] += change[i * 3 + c];
                weights[cell] += 1.0f;
            }
        }
        const unsigned r = std::max(1u, radius / kCoarse);
        for (unsigned pass = 0; pass < 3; ++pass)
        {
            BoxBlur(&sums, cw, ch, 3, r);
            BoxBlur(&weights, cw, ch, 1, r);
        }
        coarse.assign(size_t(cw) * ch * 3, 0.0f);
        for (size_t cell = 0; cell < size_t(cw) * ch; ++cell)
            for (unsigned c = 0; c < 3; ++c)
                coarse[cell * 3 + c] = weights[cell] > 1.0e-6f ? sums[cell * 3 + c] / weights[cell] : 0.0f;
    }
    double energy = 0.0, left = 0.0;
    ShiftSum sum;
    for (unsigned y = 0; y < h; ++y)
    {
        const float gy = std::min(std::max((float(y) + 0.5f) / float(kCoarse) - 0.5f, 0.0f), float(ch - 1));
        const unsigned y0 = unsigned(gy), y1 = std::min(y0 + 1, ch - 1);
        const float fy = gy - float(y0);
        for (unsigned x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            float lock[3] = { mean[0], mean[1], mean[2] };
            if (!coarse.empty())
            {
                const float gx = std::min(std::max((float(x) + 0.5f) / float(kCoarse) - 0.5f, 0.0f), float(cw - 1));
                const unsigned x0 = unsigned(gx), x1 = std::min(x0 + 1, cw - 1);
                const float fx = gx - float(x0);
                for (unsigned c = 0; c < 3; ++c)
                {
                    const float top = coarse[(size_t(y0) * cw + x0) * 3 + c] * (1.0f - fx) +
                                      coarse[(size_t(y0) * cw + x1) * 3 + c] * fx;
                    const float bottom = coarse[(size_t(y1) * cw + x0) * 3 + c] * (1.0f - fx) +
                                         coarse[(size_t(y1) * cw + x1) * 3 + c] * fx;
                    lock[c] = top * (1.0f - fy) + bottom * fy;
                }
            }
            float result[3] = { ol[i * 3], ol[i * 3 + 1], ol[i * 3 + 2] };
            if (valid[i] != 0)
            {
                const float yp = Luma(&pl[i * 3]), yo = Luma(&ol[i * 3]);
                for (unsigned c = 0; c < 3; ++c)
                {
                    const float d = change[i * 3 + c];
                    energy += double(d) * d;
                    if (all)
                        result[c] = pl[i * 3 + c] * (yo / yp); // the luminance change alone
                    else
                    {
                        result[c] = kind == kLockGains ? ol[i * 3 + c] * std::exp2(-lock[c])
                                                       : std::max(ol[i * 3 + c] - yo * lock[c], 0.0f);
                        left += double(d - lock[c]) * (d - lock[c]);
                    }
                }
                const float lum = Luma(result);
                if (kind == kLockGains && lum > 0.0f)
                    for (unsigned c = 0; c < 3; ++c)
                        result[c] *= yo / lum; // the output's own luminance: only its colour is locked
            }
            if (y % kShiftStep == 0 && x % kShiftStep == 0)
                sum.Add(pl[i * 3], pl[i * 3 + 1], pl[i * 3 + 2], result[0], result[1], result[2]);
        }
    }
    *kept = energy > 0.0 ? left / energy : 0.0;
    return sum.Result();
}

// What each capture is measured on: the model in the game, then the replays, then the locks on the replay of our
// proxy.
enum Variant
{
    kInGame,
    kOurs,
    kFinished,
    kCurve,
    kCurves,
    kDarker,
    kBrighter,
    kTone0,
    kTone05,
    kStyle1,
    kStyle2,
    kStructure0,
    kAutoMask1,
    kNeutralMask,
    kSkyTone0,
    kLockMean,
    kLock8,
    kLock32,
    kGainsMean,
    kGains8,
    kGains32,
    kColour0,
    kVariants
};

const char* const kVariantNames[kVariants] = {
    "in the game (the dump)",
    "replay: our proxy",
    "replay: the game's own picture",
    "replay: our proxy, the game's curve",
    "replay: our proxy, the game's 3 curves",
    "replay: our proxy 1 EV darker",
    "replay: our proxy 1 EV brighter",
    "replay: our proxy, LocalTone 0",
    "replay: our proxy, LocalTone 0.5",
    "replay: our proxy, Style 1",
    "replay: our proxy, Style 2",
    "replay: our proxy, LocalStructure 0",
    "replay: our proxy, AutoMask 1",
    "replay: our proxy, a mask of all 1s",
    "replay: our proxy, mask: sky at LocalTone 0",
    "lock, added: the change's mean off",
    "lock, added: low-pass over 1/8 height off",
    "lock, added: low-pass over 1/32 height off",
    "lock, gains: the change's mean off",
    "lock, gains: low-pass over 1/8 height off",
    "lock, gains: low-pass over 1/32 height off",
    "lock: all of it off (Colour 0)",
};

struct CaptureNumbers
{
    bool have[kVariants] = {};
    Shift shift[kVariants];
    double kept[kVariants] = {}; // the locks: the share of the colour change's energy left
    double sky = 0.0;            // SkyLike, of our proxy
};

// One capture: its three files read, checked against each other, compared and replayed. False when it could not be
// read.
bool OneCapture(unsigned index, const std::wstring& folder, const std::wstring& base, const Replay& replay,
                CaptureNumbers* numbers)
{
    char tag[16];
    std::snprintf(tag, sizeof tag, "C%u", index);
    const std::string name = Utf8(base);
    FrameFile f;
    if (!ReadFrameFile(folder + L"\\" + base + L".bzframe", &f))
    {
        Say("%s %s: its .bzframe could not be read", tag, name.c_str());
        return false;
    }
    Dump d;
    if (!ReadDump(folder + L"\\" + base + L".bzdump", &d))
    {
        Say("%s %s: no .bzdump beside it that this probe can read, so no white point: skipped", tag, name.c_str());
        return false;
    }
    const bool linear = (d.flags & kDumpLinearHdr) != 0;
    const float white = linear && d.white > 1.0e-20f && d.white < 1.0e20f ? d.white : 1.0f;
    const Image& frame = f.frame;
    const unsigned mw = f.modelWidth != 0 ? f.modelWidth : frame.width;
    const unsigned mh = f.modelHeight != 0 ? f.modelHeight : frame.height;
    const bool scaled = mw != frame.width || mh != frame.height;
    Image picture;
    const bool havePicture = f.pictureFormat != 0 && ReadBmp(folder + L"\\" + base + L".bmp", &picture);
    Say("%s %s (frame %llu): %ux%u %s, %s, white point %.4g, shoulder %.2f", tag, name.c_str(),
        static_cast<unsigned long long>(f.number), frame.width, frame.height, FormatName(f.format),
        linear ? "linear HDR" : "display-encoded", double(white), double(d.shoulder));
    Note("%s   the model %ux%u%s: Intensity %.2f, Style %u, LocalStructure %.2f, LocalTone %.2f, SkinStructure %.2f, "
         "AutoMask %d%s%s",
         tag, mw, mh, scaled ? " (ModelScale)" : "", double(f.t.intensity), f.t.style, double(f.t.localStructure),
         double(f.t.localTone), double(f.t.skin), f.t.autoMask,
         f.mask ? "; it had the sky sliders' mask, the replays have none" : "", f.dilate ? "; motion dilated" : "");

    // Our proxy rebuilt from the frame (at the model's size, `p`), against the dump's: the frame and the dump are of
    // the same frame, read right.
    const Image ours = EncodeProxy(frame, linear, white, d.shoulder);
    const Image p = scaled ? Resize(ours, mw, mh) : ours;
    const Image oursReduced = Reduce(scaled ? AtFramePixels(p, frame.width, frame.height) : ours, 4); // NR_DUMP_SCALE
    const double proxyDiff =
        oursReduced.width == d.width && oursReduced.height == d.height ? Difference(oursReduced.rgb, d.proxy) : 1.0e9;
    // The finished picture against the frame: the same view, or of no use here.
    double agreement = std::numeric_limits<double>::quiet_NaN();
    bool usePicture = false;
    if (havePicture)
    {
        if (picture.width != frame.width || picture.height != frame.height)
            picture = Resize(picture, frame.width, frame.height);
        agreement = Agreement(frame, picture);
        usePicture = !f.displayHdr && agreement >= kAgreementNeeded;
    }
    char pictureNote[160];
    if (!havePicture)
        std::snprintf(pictureNote, sizeof pictureNote, "%s",
                      f.pictureFormat == 0 ? "none was taken" : "its .bmp could not be read");
    else
        std::snprintf(pictureNote, sizeof pictureNote, "%s %ux%u, display %s, correlation with the frame %.3f%s",
                      FormatName(f.pictureFormat), f.pictureWidth, f.pictureHeight, f.displayHdr ? "HDR" : "SDR",
                      agreement,
                      usePicture     ? " (the same view)"
                      : f.displayHdr ? " (not used: an HDR picture)"
                                     : " (not used: another view, or it moved)");
    Note("%s   checks: our proxy rebuilt from the frame against the dump's, mean |difference| %.5f%s; the finished "
         "picture: %s",
         tag, proxyDiff, proxyDiff < 0.004 ? " (the same frame)" : " (NOT the same: what follows is doubtful)",
         pictureNote);
    if (usePicture)
        ToneTable(tag, frame, white, ours, picture);

    // The model.
    CaptureNumbers& n = *numbers;
    n.sky = SkyLike(ours);
    n.have[kInGame] = true;
    n.shift[kInGame] = ColourShift(d.proxy, d.model, d.width, 0, d.height);
    Image o;
    const bool replayed = replay(p, f.t, nullptr, &o);
    if (replayed)
    {
        n.have[kOurs] = true;
        n.shift[kOurs] = ColourShift(p.rgb, o.rgb, mw, 0, mh, kShiftStep);
        Note("%s   the replay on our proxy against the model's output in the game: mean |difference| %.4f, both "
             "reduced as the dump is",
             tag, Difference(Reduce(scaled ? AtFramePixels(o, frame.width, frame.height) : o, 4).rgb, d.model));
    }
    auto run = [&](Variant v, const Image& input, const Tunables& t, const Image* mask = nullptr)
    {
        Image result;
        if (!replay(input, t, mask, &result))
            return;
        n.have[v] = true;
        n.shift[v] = ColourShift(input.rgb, result.rgb, mw, 0, mh, kShiftStep);
    };
    if (replayed && usePicture)
    {
        run(kFinished, scaled ? Resize(picture, mw, mh) : picture, f.t);
        if (linear)
        {
            const Curve pooled = FitCurve(frame, white, picture, 3);
            const Curve each[3] = { FitCurve(frame, white, picture, 0), FitCurve(frame, white, picture, 1),
                                    FitCurve(frame, white, picture, 2) };
            if (pooled.ok)
            {
                PrintCurves(tag, pooled, each, d.shoulder);
                const Curve three[3] = { pooled, pooled, pooled };
                const Image curved = EncodeWithCurves(frame, white, three);
                run(kCurve, scaled ? Resize(curved, mw, mh) : curved, f.t);
            }
            if (each[0].ok && each[1].ok && each[2].ok)
            {
                const Image curved = EncodeWithCurves(frame, white, each);
                run(kCurves, scaled ? Resize(curved, mw, mh) : curved, f.t);
            }
        }
    }
    if (replayed && linear)
    {
        const Image darker = EncodeProxy(frame, true, white * 2.0f, d.shoulder);
        run(kDarker, scaled ? Resize(darker, mw, mh) : darker, f.t);
        const Image brighter = EncodeProxy(frame, true, white * 0.5f, d.shoulder);
        run(kBrighter, scaled ? Resize(brighter, mw, mh) : brighter, f.t);
    }
    if (replayed)
    {
        Tunables t = f.t;
        t.localTone = 0.0f;
        run(kTone0, p, t);
        t.localTone = 0.5f;
        run(kTone05, p, t);
        t = f.t;
        t.style = 1;
        run(kStyle1, p, t);
        t.style = 2;
        run(kStyle2, p, t);
        t = f.t;
        t.localStructure = 0.0f;
        run(kStructure0, p, t);
        t = f.t;
        t.autoMask = 1;
        run(kAutoMask1, p, t);
        // A control mask switches the model's own skin mask off (T8): first one that changes nothing else, then one
        // with LocalTone off where the picture looks like sky, as the sky sliders would set it from the depth.
        Image mask;
        mask.width = mw;
        mask.height = mh;
        mask.rgb.assign(mask.Pixels() * 3, 1.0f);
        run(kNeutralMask, p, f.t, &mask);
        for (size_t i = 0; i < mask.Pixels(); ++i)
        {
            const float c[3] = { Decode(p.rgb[i * 3]), Decode(p.rgb[i * 3 + 1]), Decode(p.rgb[i * 3 + 2]) };
            if (SkyLikePixel(c))
                mask.rgb[i * 3 + 1] = 0.0f;
        }
        run(kSkyTone0, p, f.t, &mask);
        const Variant locks[] = { kLockMean, kLock8, kLock32, kGainsMean, kGains8, kGains32, kColour0 };
        const LockKind kinds[] = { kLockAdded, kLockAdded, kLockAdded, kLockGains, kLockGains, kLockGains, kLockAll };
        const unsigned radii[] = { 0, mh / 8, mh / 32, 0, mh / 8, mh / 32, 0 };
        for (unsigned i = 0; i < 7; ++i)
        {
            n.have[locks[i]] = true;
            n.shift[locks[i]] = ColourLock(p, o, radii[i], kinds[i], &n.kept[locks[i]]);
        }
    }

    Note("%s   bright and blue (sky-like): %.0f%% of our proxy; the model's change of log2(blue / red), whole frame | "
         "warm pixels (%.0f%% of the frame):",
         tag, 100.0 * n.sky, 100.0 * n.shift[kInGame].warmShare);
    for (unsigned v = 0; v < kVariants; ++v)
    {
        if (!n.have[v])
            continue;
        if (v >= kLockMean)
            Note("%s     %-44s %+.3f | %+.3f   colour change kept %3.0f%%", tag, kVariantNames[v], n.shift[v].all,
                 n.shift[v].warm, 100.0 * n.kept[v]);
        else
            Note("%s     %-44s %+.3f | %+.3f", tag, kVariantNames[v], n.shift[v].all, n.shift[v].warm);
    }
    if (!replayed)
        Note("%s     the replays failed (above)", tag);

    // The bottom half alone: if the sky above it is what cools the ground, the ground alone comes back less cooled.
    const unsigned top = mh / 2;
    Image alone;
    const Image bottom = Rows(p, top, mh);
    if (replayed && replay(bottom, f.t, nullptr, &alone))
    {
        const Shift inFrame = ColourShift(p.rgb, o.rgb, mw, top, mh, kShiftStep);
        const Shift byItself = ColourShift(bottom.rgb, alone.rgb, mw, 0, mh - top, kShiftStep);
        Note("%s   the bottom half of our proxy: in the whole frame %+.3f | %+.3f, alone %+.3f | %+.3f", tag,
             inFrame.all, inFrame.warm, byItself.all, byItself.warm);
    }
    return true;
}

// Over all the captures: per variant the mean change on warm pixels, its least and most, and its range, which is
// the drift from one view to the next; then whether the replays stand for the game, and where the drift comes from.
void CaptureSummary(const std::vector<CaptureNumbers>& all)
{
    if (all.empty())
        return;
    Say("captures: over %u, the model's change of log2(blue / red) on warm pixels: mean, least .. most, and the range "
        "(the drift from one view to the next)",
        unsigned(all.size()));
    double range[kVariants] = {};
    unsigned count[kVariants] = {};
    for (unsigned v = 0; v < kVariants; ++v)
    {
        double sum = 0.0, sumAll = 0.0, kept = 0.0, least = 1.0e9, most = -1.0e9;
        for (const CaptureNumbers& c : all)
        {
            if (!c.have[v])
                continue;
            sum += c.shift[v].warm;
            sumAll += c.shift[v].all;
            kept += c.kept[v];
            least = std::min(least, c.shift[v].warm);
            most = std::max(most, c.shift[v].warm);
            ++count[v];
        }
        if (count[v] == 0)
            continue;
        range[v] = most - least;
        const double k = double(count[v]);
        if (v >= kLockMean)
            Note("captures   %-44s n %2u  mean %+.3f  %+.3f .. %+.3f  range %.3f  (whole frame %+.3f)  kept %3.0f%%",
                 kVariantNames[v], count[v], sum / k, least, most, range[v], sumAll / k, 100.0 * kept / k);
        else
            Note("captures   %-44s n %2u  mean %+.3f  %+.3f .. %+.3f  range %.3f  (whole frame %+.3f)",
                 kVariantNames[v], count[v], sum / k, least, most, range[v], sumAll / k);
    }
    double difference = 0.0;
    unsigned both = 0;
    for (const CaptureNumbers& c : all)
    {
        if (c.have[kInGame] && c.have[kOurs])
        {
            difference += std::fabs(c.shift[kOurs].warm - c.shift[kInGame].warm);
            ++both;
        }
    }
    if (both != 0)
        Note("captures   the replay on our proxy against the game, warm pixels: mean |difference| %.3f over %u: %s",
             difference / double(both), both,
             difference / double(both) < 0.15 ? "close, the replays stand for the game"
                                              : "NOT close, the replays say less about the game");
    if (count[kOurs] >= 2 && count[kFinished] >= 2 && range[kOurs] > 0.05)
    {
        const double share = range[kFinished] / range[kOurs];
        Note("captures   the drift with the game's own picture as the input is %.0f%% of that with our proxy: %s",
             100.0 * share,
             share < 0.35  ? "it comes mostly from our proxy"
             : share > 0.7 ? "it is mostly the model's own"
                           : "both have a part");
    }
    else
        Note("captures   no verdict on where the drift comes from: that needs two captures or more with the finished "
             "picture, and a drift on our proxy");
}

// Every capture in `folder` (its .bzframe files, oldest first: the names carry the time), then the summary.
void Captures(const std::wstring& folder, const Replay& replay)
{
    std::vector<std::wstring> bases;
    WIN32_FIND_DATAW found;
    const HANDLE find = FindFirstFileW((folder + L"\\*.bzframe").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring file = found.cFileName;
            if (file.size() > 8 && file.compare(file.size() - 8, 8, L".bzframe") == 0)
                bases.push_back(file.substr(0, file.size() - 8));
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (bases.empty())
    {
        Say("captures: no .bzframe files in %s", Utf8(folder).c_str());
        return;
    }
    std::sort(bases.begin(), bases.end());
    Say("captures: %u in %s; the model's change is in log2(blue / red): bluer +, warmer -", unsigned(bases.size()),
        Utf8(folder).c_str());
    std::vector<CaptureNumbers> all;
    unsigned index = 0;
    for (const std::wstring& base : bases)
    {
        CaptureNumbers numbers;
        if (OneCapture(++index, folder, base, replay, &numbers))
            all.push_back(numbers);
    }
    CaptureSummary(all);
}

// The model on pictures of our own, for --captures: a feature per size (few: a process may only create so many),
// kept for every capture of that size, with its Color, Depth (flat: 310.8 never reads it), MVec (still) and Output
// textures. Each run starts from a reset and evaluates the same picture kReplayFrames times, as T6 does, so that the
// model's history settles on it.
struct ReplaySlot
{
    void* feature = nullptr;
    NVSDK_NGX_Parameter* block = nullptr;
    Inputs in;
    ID3D12Resource* mask = nullptr; // made the first time a mask is given
};

void DropSlot(const Model& m, ReplaySlot* s)
{
    if (s->feature != nullptr)
        m.release(s->feature);
    for (ID3D12Resource* r : { s->in.colour, s->in.depth, s->in.motion, s->in.output, s->mask })
        if (r != nullptr)
            r->Release();
    *s = ReplaySlot();
}

bool MakeSlot(const Model& m, unsigned w, unsigned h, ReplaySlot* s)
{
    const std::vector<float> flat(size_t(w) * h, 0.5f);
    const std::vector<uint16_t> still(size_t(w) * h * 2, FloatToHalf(0.0f));
    Inputs& in = s->in;
    in.width = in.depthWidth = in.motionWidth = w;
    in.height = in.depthHeight = in.motionHeight = h;
    in.mvScaleX = float(w);
    in.mvScaleY = float(h);
    in.colour = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    in.depth = MakeFilled(DXGI_FORMAT_R32_FLOAT, w, h, flat.data(), w * 4);
    in.motion = MakeFilled(DXGI_FORMAT_R16G16_FLOAT, w, h, still.data(), w * 4);
    in.output = MakeOutput(w, h);
    if (in.colour == nullptr || in.depth == nullptr || in.motion == nullptr || in.output == nullptr)
    {
        Say("    no textures of %ux%u for the replays", w, h);
        return false;
    }
    s->feature = CreateOwn(m, w, h, nullptr, &s->block);
    return s->feature != nullptr;
}

bool ReplayOn(const Model& m, std::vector<ReplaySlot>* slots, const Image& input, const Tunables& t, const Image* mask,
              Image* output)
{
    ReplaySlot* slot = nullptr;
    for (ReplaySlot& s : *slots)
        if (s.in.width == input.width && s.in.height == input.height)
            slot = &s;
    if (slot == nullptr)
    {
        if (slots->size() >= 3)
        {
            DropSlot(m, &slots->front());
            slots->erase(slots->begin());
        }
        slots->emplace_back();
        if (!MakeSlot(m, input.width, input.height, &slots->back()))
        {
            DropSlot(m, &slots->back());
            slots->pop_back();
            return false;
        }
        slot = &slots->back();
    }
    const std::vector<uint16_t> rgba = ToHalfRgba(input.rgb, 0, input.Pixels());
    if (!UploadNow(slot->in.colour, rgba.data(), input.width * 8, input.height,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        Say("    the replay's picture could not be uploaded");
        return false;
    }
    if (mask != nullptr)
    {
        if (slot->mask == nullptr)
            slot->mask = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, input.width, input.height,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const std::vector<uint16_t> texels = ToHalfRgba(mask->rgb, 0, mask->Pixels());
        if (slot->mask == nullptr ||
            !UploadNow(slot->mask, texels.data(), input.width * 8, input.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        {
            Say("    the replay's mask could not be made");
            return false;
        }
        SetMask(slot->block, slot->mask, input.width, input.height);
    }
    output->width = input.width;
    output->height = input.height;
    const bool ran = Run(m, slot->feature, slot->block, slot->in, &t, kReplayFrames, &output->rgb);
    if (mask != nullptr)
        SetMask(slot->block, nullptr, 0, 0);
    return ran;
}
} // namespace

constexpr int kFaulted = -1;

// Calls a shutdown function under a structured exception handler. Its own function because SEH cannot share a
// frame with objects that need unwinding.
template <typename Function, typename Argument> int Guarded(Function function, Argument argument)
{
    __try
    {
        return int(function(argument));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return kFaulted;
    }
}

// NvAPI as a game holds it: nvapi64.dll loaded and NvAPI_Initialize called by the exe itself, never unloaded.
// nvapi_QueryInterface(0x0150E828) is NvAPI_Initialize; 0 is NVAPI_OK.
using NvapiQueryInterface = void*(__cdecl*)(unsigned int);
using NvapiInitialize = int(__cdecl*)();

void InitialiseNvapiFirst()
{
    const HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    const auto query = nvapi != nullptr ? Proc<NvapiQueryInterface>(nvapi, "nvapi_QueryInterface") : nullptr;
    const auto initialize = query != nullptr ? reinterpret_cast<NvapiInitialize>(query(0x0150E828u)) : nullptr;
    if (initialize == nullptr)
    {
        Say("NvAPI first: nvapi64.dll or its NvAPI_Initialize was not found (%lu)", GetLastError());
        return;
    }
    const int status = initialize();
    Say("NvAPI first: NvAPI_Initialize by nrprobe.exe itself, before anything else -> %d%s", status,
        status == 0 ? " (NVAPI_OK; held for the life of the process, as a game holds it)" : "");
}

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, &options))
        return Fail("usage: nrprobe [--model <path>] [--size WxH] [--evaluates N] [--init N] [--capability]\n"
                    "               [--no-model-shutdown] [--no-core-shutdown] [--nvapi-first] [--tuning]\n"
                    "               [--dumps <folder>] [--subrects] [--captures <folder>]");
    if (options.model.empty())
    {
        const wchar_t* const known[] = { L"D:\\Program Files\\Epic Games\\TheWitcher3\\bin\\x64_dx12\\nvngx_dlssnr.dll",
                                         L"D:\\Program Files\\The Last of Us Part II Remastered\\nvngx_dlssnr.dll" };
        for (const wchar_t* path : known)
        {
            if (FileExists(path))
            {
                options.model = path;
                break;
            }
        }
    }
    if (options.model.empty() || !FileExists(options.model))
        return Fail("no model: give --model <path\\nvngx_dlssnr.dll>");
    Say("model: %s", Utf8(options.model).c_str());
    Say("frame: %ux%u, %u evaluations", options.width, options.height, options.evaluates);
    Say("teardown: model Shutdown1 %s, core Shutdown1 %s, NvAPI initialised by this exe first: %s",
        options.modelShutdown ? "yes" : "no (--no-model-shutdown)",
        options.coreShutdown ? "yes" : "no (--no-core-shutdown)", options.nvapiFirst ? "yes" : "no");
    if (options.nvapiFirst)
        InitialiseNvapiFirst();

    // The device.
    if (!CreateGpu())
        return Fail("no D3D12 device on an NVIDIA adapter");
    Say("device: created");

    // The driver's core, initialised as a game would, so that its AllocateParameters works.
    const std::wstring corePath = FindCore();
    if (corePath.empty())
        return Fail("the driver's _nvngx.dll was not found");
    const HMODULE core = LoadLibraryW(corePath.c_str());
    if (core == nullptr)
        return Fail("%s did not load (%lu)", Utf8(corePath).c_str(), GetLastError());
    Say("core: %s", Utf8(corePath).c_str());
    const auto coreInit = Proc<CoreInitExt>(core, "NVSDK_NGX_D3D12_Init_Ext");
    const auto getCapability = Proc<GetParams>(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    const auto allocate = Proc<GetParams>(core, "NVSDK_NGX_D3D12_AllocateParameters");
    const auto shutdown = Proc<Shutdown1>(core, "NVSDK_NGX_D3D12_Shutdown1");
    if (coreInit == nullptr || getCapability == nullptr || allocate == nullptr)
        return Fail("the core lacks Init_Ext, GetCapabilityParameters or AllocateParameters");
    const std::wstring ourData = OurDataPath();
    wchar_t temp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, temp);
    const std::wstring coreData = !ourData.empty() ? ourData : std::wstring(temp);
    static NVSDK_NGX_FeatureCommonInfo coreInfo = {};
    coreInfo.LoggingInfo.LoggingCallback = &OnNgxLog;
    coreInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    coreInfo.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result result = coreInit(0x42414E41ull, coreData.c_str(), g.device, NVSDK_NGX_Version_API, &coreInfo);
    Say("core Init_Ext(app id 0x42414E41, %s) -> 0x%08X %s", Utf8(coreData).c_str(), unsigned(result),
        ResultName(int(result)));
    if (result != NVSDK_NGX_Result_Success)
        return Fail("the core would not initialise");
    NVSDK_NGX_Parameter* capability = nullptr;
    result = getCapability(&capability);
    Say("core GetCapabilityParameters -> 0x%08X %s, block %p", unsigned(result), ResultName(int(result)),
        static_cast<void*>(capability));

    // The bridge, from beside this exe, and the model through it.
    const std::wstring bridgePath = ExeFolder() + L"\\banana.nvngx.dll";
    const HMODULE bridge = LoadLibraryW(bridgePath.c_str());
    if (bridge == nullptr)
        return Fail("%s did not load (%lu); build the solution first", Utf8(bridgePath).c_str(), GetLastError());
    const auto bzLoad = Proc<BzLoad>(bridge, "bz_load");
    const auto bzInit = Proc<BzInit>(bridge, "bz_init");
    const auto bzPopulate = Proc<BzPopulate>(bridge, "bz_populate");
    const auto bzCreate = Proc<BzCreate>(bridge, "bz_create");
    const auto bzEvaluate = Proc<BzEvaluate>(bridge, "bz_evaluate");
    const auto bzRelease = Proc<BzRelease>(bridge, "bz_release");
    const auto bzShutdown = Proc<BzShutdown>(bridge, "bz_shutdown");
    const auto bzBuild = Proc<BzBuild>(bridge, "bz_build");
    if (bzLoad == nullptr || bzInit == nullptr || bzPopulate == nullptr || bzCreate == nullptr ||
        bzEvaluate == nullptr || bzRelease == nullptr || bzShutdown == nullptr || bzBuild == nullptr)
        return Fail("%s lacks the bz_* exports", Utf8(bridgePath).c_str());
    Say("bridge: %s, build %s", Utf8(bridgePath).c_str(), bzBuild());
    const int found = bzLoad(options.model.c_str());
    Say("bz_load(model) -> 0x%X%s", unsigned(found), found == 63 ? " (all six entry points)" : "");
    if (found != 63)
        return Fail("the model did not load through the bridge (%lu)", GetLastError());

    // The Init_Ext ladder, or one rung of it.
    static NVSDK_NGX_FeatureCommonInfo withLog = {};
    withLog.LoggingInfo.LoggingCallback = &OnNgxLog;
    withLog.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    withLog.LoggingInfo.DisableOtherLoggingSinks = true;
    static const NVSDK_NGX_FeatureCommonInfo zeroed = {};
    struct Attempt
    {
        const char* name;
        unsigned long long appId;
        std::wstring dataPath;
        const void* info;
    };
    const Attempt attempts[] = {
        { "own app id, own data dir, zeroed info with our log callback", 0x42414E41ull, coreData, &withLog },
        { "own app id, own data dir, zeroed info", 0x42414E41ull, coreData, &zeroed },
        { "old app id, own data dir, zeroed info", 0x24480451ull, coreData, &zeroed },
        { "old app id, %TEMP%, zeroed info", 0x24480451ull, temp, &zeroed },
        { "old app id, %TEMP%, the capability block as the 5th argument", 0x24480451ull, temp, capability },
    };
    const char* accepted = nullptr;
    for (int i = 0; i < 5; ++i)
    {
        if (options.init != 0 && options.init != i + 1)
            continue;
        const Attempt& a = attempts[i];
        if (a.info == nullptr)
            continue;
        const int r = bzInit(a.appId, a.dataPath.c_str(), g.device, NVSDK_NGX_Version_API, a.info);
        Say("model Init_Ext attempt %d (%s, app id 0x%llX, %s) -> 0x%08X %s", i + 1, a.name, a.appId,
            Utf8(a.dataPath).c_str(), unsigned(r), ResultName(r));
        if (r == NVSDK_NGX_Result_Success)
        {
            accepted = a.name;
            break;
        }
    }
    if (accepted == nullptr)
        return Fail("the model refused Init_Ext");
    Say("model initialised with: %s", accepted);

    // Parameter blocks.
    NVSDK_NGX_Parameter* createBlock = nullptr;
    NVSDK_NGX_Parameter* evalBlock = nullptr;
    bool own = false;
    if (!options.capability)
    {
        result = allocate(&createBlock);
        const NVSDK_NGX_Result second = result == NVSDK_NGX_Result_Success ? allocate(&evalBlock) : result;
        Say("core AllocateParameters -> 0x%08X %s, 0x%08X %s", unsigned(result), ResultName(int(result)),
            unsigned(second), ResultName(int(second)));
        own = second == NVSDK_NGX_Result_Success && createBlock != nullptr && evalBlock != nullptr;
    }
    if (!own)
    {
        if (capability == nullptr)
            return Fail("no parameter block at all");
        createBlock = evalBlock = capability;
    }
    Say("parameter blocks: %s", own ? "two of our own" : "the core's capability block for both");
    int populated = bzPopulate(createBlock);
    Say("model PopulateParameters_Impl(create block) -> 0x%08X %s", unsigned(populated), ResultName(populated));
    if (own)
    {
        populated = bzPopulate(evalBlock);
        Say("model PopulateParameters_Impl(evaluate block) -> 0x%08X %s", unsigned(populated), ResultName(populated));
    }

    // The synthetic frame: a gradient with fine detail in the middle, a depth ramp, no motion.
    const unsigned w = options.width, h = options.height;
    std::vector<uint16_t> colour(size_t(w) * h * 4);
    std::vector<float> depth(size_t(w) * h);
    std::vector<uint16_t> motion(size_t(w) * h * 2, 0);
    for (unsigned y = 0; y < h; ++y)
    {
        for (unsigned x = 0; x < w; ++x)
        {
            float r = float(x) / float(w - 1);
            float gr = float(y) / float(h - 1);
            float b = 0.5f + 0.25f * float(std::sin(x * 0.05) * std::cos(y * 0.05));
            if (x >= w / 4 && x < 3 * w / 4 && ((x / 2 + y / 2) & 1u) != 0)
            {
                r += 0.04f;
                gr += 0.04f;
                b += 0.04f;
            }
            uint16_t* pixel = &colour[(size_t(y) * w + x) * 4];
            pixel[0] = FloatToHalf(r);
            pixel[1] = FloatToHalf(gr);
            pixel[2] = FloatToHalf(b);
            pixel[3] = FloatToHalf(1.0f);
            depth[size_t(y) * w + x] = 0.3f + 0.6f * float(y) / float(h);
        }
    }
    ID3D12Resource* colourTexture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* depthTexture =
        MakeTexture(DXGI_FORMAT_R32_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motionTexture =
        MakeTexture(DXGI_FORMAT_R16G16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* outputTexture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (colourTexture == nullptr || depthTexture == nullptr || motionTexture == nullptr || outputTexture == nullptr)
        return Fail("could not create the textures");
    if (!Upload(colourTexture, colour.data(), w * 8, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
        !Upload(depthTexture, depth.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
        !Upload(motionTexture, motion.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
        Submit("upload") < 0.0)
        return Fail("could not upload the frame");
    Say("frame uploaded: Color RGBA16F, Depth R32_FLOAT, MVec R16G16_FLOAT, Output RGBA16F (UAV)");

    // Create, on the command list, then run that list: the model records its set-up into it.
    createBlock->Set("DLSSNR.Enabled", 1u);
    createBlock->Set("DLSSNR.Width", w);
    createBlock->Set("DLSSNR.Height", h);
    createBlock->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    createBlock->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    void* handle = nullptr;
    int created = bzCreate(g.list, 18, createBlock, &handle);
    Say("model CreateFeature(18, %ux%u, %s) -> 0x%08X %s, handle %p", w, h, own ? "own block" : "capability block",
        unsigned(created), ResultName(created), handle);
    if (created != NVSDK_NGX_Result_Success && own && capability != nullptr)
    {
        populated = bzPopulate(capability);
        capability->Set("DLSSNR.Enabled", 1u);
        capability->Set("DLSSNR.Width", w);
        capability->Set("DLSSNR.Height", h);
        capability->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        capability->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        handle = nullptr;
        created = bzCreate(g.list, 18, capability, &handle);
        Say("model CreateFeature(18, %ux%u, capability block after PopulateParameters_Impl -> 0x%08X) -> 0x%08X %s, "
            "handle %p",
            w, h, unsigned(populated), unsigned(created), ResultName(created), handle);
        if (created == NVSDK_NGX_Result_Success)
        {
            createBlock = evalBlock = capability; // our two blocks stay allocated, as in dxgi.dll
            own = false;
        }
    }
    if (created != NVSDK_NGX_Result_Success || handle == nullptr)
        return Fail("the model would not create its feature");
    const double createMs = Submit("create");
    if (createMs < 0.0)
        return Fail("the creation commands did not run");
    Say("creation commands ran on the GPU (%.1f ms including the wait)", createMs);

    // Evaluations.
    int failures = 0;
    for (unsigned i = 0; i < options.evaluates; ++i)
    {
        NVSDK_NGX_Parameter* p = evalBlock;
        p->Set("DLSSNR.Color", colourTexture);
        p->Set("DLSSNR.Depth", depthTexture);
        p->Set("DLSSNR.MVec", motionTexture);
        p->Set("DLSSNR.Output", outputTexture);
        p->Set("DLSSNR.Enabled", 1u);
        p->Set("DLSSNR.Width", w);
        p->Set("DLSSNR.Height", h);
        p->Set("DLSSNR.DepthInverted", 0u);
        p->Set("DLSSNR.Reset", i == 0 ? 1u : 0u);
        p->Set("DLSSNR.ColorSubrectBaseX", 0u);
        p->Set("DLSSNR.ColorSubrectBaseY", 0u);
        p->Set("DLSSNR.ColorSubrectWidth", w);
        p->Set("DLSSNR.ColorSubrectHeight", h);
        p->Set("DLSSNR.OutputSubrectBaseX", 0u);
        p->Set("DLSSNR.OutputSubrectBaseY", 0u);
        p->Set("DLSSNR.OutputSubrectWidth", w);
        p->Set("DLSSNR.OutputSubrectHeight", h);
        p->Set("DLSSNR.DepthSubrectBaseX", 0u);
        p->Set("DLSSNR.DepthSubrectBaseY", 0u);
        p->Set("DLSSNR.DepthSubrectWidth", w);
        p->Set("DLSSNR.DepthSubrectHeight", h);
        p->Set("DLSSNR.MVecSubrectBaseX", 0u);
        p->Set("DLSSNR.MVecSubrectBaseY", 0u);
        p->Set("DLSSNR.MVecSubrectWidth", w);
        p->Set("DLSSNR.MVecSubrectHeight", h);
        p->Set("DLSSNR.MVecScaleX", 1.0f);
        p->Set("DLSSNR.MVecScaleY", 1.0f);
        const int evaluated = bzEvaluate(g.list, handle, p, nullptr);
        const double ms = Submit("evaluate");
        Say("model EvaluateFeature %u -> 0x%08X %s%s%.1f ms", i + 1, unsigned(evaluated), ResultName(evaluated),
            ms < 0.0 ? "; GPU: failed, " : ", ", ms < 0.0 ? 0.0 : ms);
        if (evaluated != NVSDK_NGX_Result_Success || ms < 0.0)
            ++failures;
    }

    // What came out.
    std::vector<uint8_t> out, in;
    unsigned outPitch = 0, inPitch = 0;
    if (Readback(outputTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &out, &outPitch) &&
        Readback(colourTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &in, &inPitch))
    {
        double sumOut = 0.0, sumIn = 0.0, sumDiff = 0.0, maxDiff = 0.0;
        unsigned long long changed = 0, bad = 0;
        for (unsigned y = 0; y < h; ++y)
        {
            const auto* outRow = reinterpret_cast<const uint16_t*>(out.data() + size_t(y) * outPitch);
            const auto* inRow = reinterpret_cast<const uint16_t*>(in.data() + size_t(y) * inPitch);
            for (unsigned x = 0; x < w; ++x)
            {
                bool pixelChanged = false;
                for (unsigned c = 0; c < 3; ++c)
                {
                    const float o = HalfToFloat(outRow[x * 4 + c]);
                    const float v = HalfToFloat(inRow[x * 4 + c]);
                    if (!(o == o) || std::fabs(o) > 65504.0f)
                    {
                        ++bad;
                        continue;
                    }
                    const double d = std::fabs(double(o) - double(v));
                    sumOut += o;
                    sumIn += v;
                    sumDiff += d;
                    if (d > maxDiff)
                        maxDiff = d;
                    if (d > 1.0 / 255.0)
                        pixelChanged = true;
                }
                if (pixelChanged)
                    ++changed;
            }
        }
        const double samples = double(w) * double(h) * 3.0;
        Say("output: mean %.4f (input %.4f), mean |diff| %.5f, max |diff| %.4f, %.1f%% of pixels changed by more "
            "than 1/255, %llu NaN/Inf samples",
            sumOut / samples, sumIn / samples, sumDiff / samples, maxDiff,
            100.0 * double(changed) / (double(w) * double(h)), bad);
    }
    else
        Say("output: could not be read back");

    if ((options.tuning || options.subrects || !options.captures.empty()) && failures == 0)
    {
        Model model;
        model.populate = bzPopulate;
        model.create = bzCreate;
        model.evaluate = bzEvaluate;
        model.release = bzRelease;
        model.allocate = own ? allocate : nullptr;
        Inputs basic; // the basic run's textures
        basic.width = basic.depthWidth = basic.motionWidth = w;
        basic.height = basic.depthHeight = basic.motionHeight = h;
        basic.colour = colourTexture;
        basic.depth = depthTexture;
        basic.motion = motionTexture;
        basic.output = outputTexture;
        if (options.tuning)
        {
            Tuning(model, handle, evalBlock, basic, options.dumps);
            Say("tuning: %d of the checks failed", g_mustFail);
        }
        if (options.subrects)
        {
            if (model.allocate == nullptr)
                Say("subrects: skipped, they need parameter blocks of our own (not --capability)");
            else
            {
                Subrects9(model, handle, evalBlock, basic);
                Subrects10(model, handle, evalBlock, basic);
            }
        }
        if (!options.captures.empty())
        {
            if (model.allocate == nullptr)
                Say("captures: skipped, they need parameter blocks of our own (not --capability)");
            else
            {
                std::vector<ReplaySlot> slots;
                Captures(options.captures, [&](const Image& input, const Tunables& t, const Image* mask, Image* output)
                         { return ReplayOn(model, &slots, input, t, mask, output); });
                for (ReplaySlot& s : slots)
                    DropSlot(model, &s);
            }
        }
    }

    const int released = bzRelease(handle);
    Say("model ReleaseFeature -> 0x%08X %s", unsigned(released), ResultName(released));
    // The verdict is about the model path: creation and evaluation. The teardown after it is dxgi.dll's when a game
    // shuts NGX down (the model's own Shutdown1, then the core's), each step guarded and on its own line; our two
    // parameter blocks stay allocated, as dxgi.dll leaves them (DestroyParameters before the core's Shutdown1 faults
    // inside the core). Whether the process then exits cleanly shows in its exit code.
    if (failures == 0 && g_mustFail == 0)
        Say("PASS");
    else if (failures != 0)
        Say("FAIL (%d evaluations failed)", failures);
    else
        Say("FAIL (%d tuning checks failed)", g_mustFail);
    if (options.modelShutdown)
    {
        const int down = Guarded(bzShutdown, static_cast<void*>(g.device));
        if (down == kFaulted)
            Say("model Shutdown1 faulted (an access violation inside the model)");
        else
            Say("model Shutdown1 -> 0x%08X %s", unsigned(down), ResultName(down));
    }
    else
        Say("model Shutdown1 skipped (--no-model-shutdown): the model is left to its own DLL_PROCESS_DETACH");
    if (!options.coreShutdown)
        Say("core Shutdown1 skipped (--no-core-shutdown)");
    else if (shutdown != nullptr)
    {
        const int down = Guarded(shutdown, g.device);
        if (down == kFaulted)
            Say("core Shutdown1 faulted (an access violation inside the core)");
        else
            Say("core Shutdown1 -> 0x%08X %s%s", unsigned(down), ResultName(down),
                own ? " (our two parameter blocks left allocated, as dxgi.dll does)" : "");
    }
    const int exitCode = failures == 0 && g_mustFail == 0 ? 0 : 1;
    Say("exiting: the exit code is %d unless a DLL faults on the way out", exitCode);
    return exitCode;
}
