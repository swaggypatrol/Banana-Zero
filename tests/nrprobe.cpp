// nrprobe: does the real NR model accept our bridge, our Init_Ext arguments and our own parameter blocks, and does
// it produce a picture? A standalone console program: no game, no injection, nothing written to a game directory.
// Needs the NVIDIA driver, an RTX 50 and the user's copy of nvngx_dlssnr.dll (read only).
//
//   build\Release\nrprobe.exe [--model <path\nvngx_dlssnr.dll>] [--size WxH] [--evaluates N] [--init N]
//                             [--capability] [--no-model-shutdown] [--no-core-shutdown] [--nvapi-first]
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
// dxgi.dll from the same folder is loaded too (this exe imports dxgi), so its dlssnr.log appears beside it; it sees
// no SR/RR evaluation here and does nothing.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

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
        else
            return false;
    }
    return true;
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
                    "               [--no-model-shutdown] [--no-core-shutdown] [--nvapi-first]");
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

    const int released = bzRelease(handle);
    Say("model ReleaseFeature -> 0x%08X %s", unsigned(released), ResultName(released));
    // The verdict is about the model path: creation and evaluation. The teardown after it is dxgi.dll's when a game
    // shuts NGX down (the model's own Shutdown1, then the core's), each step guarded and on its own line; our two
    // parameter blocks stay allocated, as dxgi.dll leaves them (DestroyParameters before the core's Shutdown1 faults
    // inside the core). Whether the process then exits cleanly shows in its exit code.
    if (failures == 0)
        Say("PASS");
    else
        Say("FAIL (%d evaluations failed)", failures);
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
    Say("exiting: the exit code is %d unless a DLL faults on the way out", failures == 0 ? 0 : 1);
    return failures == 0 ? 0 : 1;
}
