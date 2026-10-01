// nrprobe: does the real NR model accept our bridge, our Init_Ext arguments and our own parameter blocks, and does
// it produce a picture? A standalone console program: no game, no injection, nothing written to a game directory.
// Needs the NVIDIA driver, an RTX 50 and the user's copy of nvngx_dlssnr.dll (read only).
//
//   build\Release\nrprobe.exe [--model <path\nvngx_dlssnr.dll>] [--size WxH] [--evaluates N] [--init N]
//                             [--capability] [--no-model-shutdown] [--no-core-shutdown] [--nvapi-first]
//                             [--tuning] [--dumps <folder>]
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
// --tuning then measures, on the same feature, what the model does with its parameters and its motion vectors
// (Tuning, below: T0 to T6): that its six parameters act when written at every evaluation and not when written only
// at creation, the model's defaults, its clamps, which motion-vector scale it wants, and on Witcher 3 frames from
// dxgi.dll's own dumps (--dumps, else %LOCALAPPDATA%\Banana-Zero\dumps) how each setting moves its colour. The
// exit code is then also 1 when one of its checks fails.
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
    std::wstring dumps; // --tuning's T6: where the frame dumps are; empty: %LOCALAPPDATA%\Banana-Zero\dumps
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
        else if (arg == L"--dumps" && hasValue)
            options->dumps = argv[++i];
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

Shift ColourShift(const std::vector<float>& before, const std::vector<float>& after, unsigned width, unsigned y0,
                  unsigned y1)
{
    double sumAll = 0.0, sumWarm = 0.0;
    unsigned long long nAll = 0, nWarm = 0;
    const double eps = 1.0e-3;
    for (unsigned y = y0; y < y1; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            const size_t i = (size_t(y) * width + x) * 3;
            const double pr = SrgbDecode(before[i]), pg = SrgbDecode(before[i + 1]), pb = SrgbDecode(before[i + 2]);
            const double orr = SrgbDecode(after[i]), og = SrgbDecode(after[i + 1]), ob = SrgbDecode(after[i + 2]);
            const double yp = 0.2126 * pr + 0.7152 * pg + 0.0722 * pb;
            const double pmax = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
            const double pmin = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
            const double omax = orr > og ? (orr > ob ? orr : ob) : (og > ob ? og : ob);
            if (!(yp > 0.01) || pmax >= 0.97 || !(omax < 0.97) || !(orr >= 0.0) || !(ob >= 0.0))
                continue;
            const double d = std::log2((ob + eps) / (orr + eps)) - std::log2((pb + eps) / (pr + eps));
            sumAll += d;
            ++nAll;
            if (pr > pg && pg > pb && (pmax - pmin) / pmax > 0.15)
            {
                sumWarm += d;
                ++nWarm;
            }
        }
    }
    Shift s;
    s.all = nAll != 0 ? sumAll / double(nAll) : 0.0;
    s.warm = nWarm != 0 ? sumWarm / double(nWarm) : 0.0;
    s.warmShare = nAll != 0 ? double(nWarm) / double(nAll) : 0.0;
    return s;
}

// A frame dump from dxgi.dll (nr_dx12.cpp, DumpHeader; tools in the M4 docs read the same): the whole frame reduced
// four times, as the model saw it (the proxy) and as it came back (the model's output), display-encoded RGB.
struct Dump
{
    unsigned width = 0, height = 0;
    std::vector<float> proxy, model;
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
    for (int which = 1; which <= 2; ++which)
    {
        std::vector<float>& out = which == 1 ? d->proxy : d->model;
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
                    "               [--dumps <folder>]");
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

    if (options.tuning && failures == 0)
    {
        Model model;
        model.populate = bzPopulate;
        model.create = bzCreate;
        model.evaluate = bzEvaluate;
        model.release = bzRelease;
        model.allocate = own ? allocate : nullptr;
        Inputs in;
        in.width = in.depthWidth = in.motionWidth = w;
        in.height = in.depthHeight = in.motionHeight = h;
        in.colour = colourTexture;
        in.depth = depthTexture;
        in.motion = motionTexture;
        in.output = outputTexture;
        Tuning(model, handle, evalBlock, in, options.dumps);
        Say("tuning: %d of the checks failed", g_mustFail);
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
