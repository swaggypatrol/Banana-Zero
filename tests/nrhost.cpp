// nrhost: a game for the closed loop. It does what a DX12 game with DLSS does, on this PC's GPU and without a window:
// the driver's NGX core initialised, DLSS Super Resolution created at DLAA (render size = output size) and evaluated
// once a frame over pictures read from files, so that dxgi.dll beside it (this exe imports dxgi) runs its NR pass as
// in a game, with the model beside it (build\Release\nvngx_dlssnr.dll, the user's own copy). Two SR features run
// on the same inputs every frame: dxgi.dll follows the first one it sees created, so the first comes out after NR
// and the second is DLSS alone. Each captured frame is written both ways, from one run.
//
//   build\Release\nrhost.exe --out <folder> [--snippets <folder holding nvngx_dlss.dll>] [--sdr]
//                            [--exposure X] [--pre-exposure X] [--fps F] [--cuts] [--frames N]
//                            [--capture last|all|I,J,...] [--set Key=Value]... <picture.pfm>[*frames]...
//
// DLSS's own DLL is looked for in --snippets, else in the snippets folder beside the exe (tools/nrloop.py prepare
// puts one there): a folder holding nvngx_dlss.dll alone, since the core loads every NGX DLL it finds there.
//
// Pictures are PFM files (colour or grey, any byte order), all of one size, holding the values the Output texture
// gets: linear light for HDR (RGBA16F, the IsHDR flag), the display-encoded values 0..1 for --sdr (RGBA8 UNORM). Each
// is shown for its *frames, else --frames (90): DLSS and the model both have a history, and NR starts some frames
// after the first evaluation. Frame 0 is a Reset; with --cuts so is the first frame of every picture. --fps paces the
// frames on the wall clock (the scene meter's speeds are in seconds); without it they go as fast as the GPU does.
// --exposure gives DLSS (and so dxgi.dll) a 1x1 exposure texture holding X; without it DLSS has AutoExposure.
// --pre-exposure passes DLSS.Pre.Exposure.
//
// Before anything else it writes dlssnr.ini beside itself, replacing what was there: Badge=0, StatsLog=0, then each
// --set in order (a later line wins), so a run's settings are exactly the ones it was given.
//
// Writes <out>\NNNN.nr.pfm (after NR) and <out>\NNNN.dlss.pfm (DLSS alone) for each captured frame NNNN (default:
// the last frame of each picture), as the Output holds them. dlssnr.log beside the exe says what NR did. Exit code 0
// when every evaluation succeeded; whether NR ran is for the log and the two pictures to say.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
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
    return 2;
}

std::string Utf8(const std::wstring& text)
{
    std::string out(text.size() * 3 + 1, '\0');
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    out.resize(length > 0 ? size_t(length) : 0);
    return out;
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

std::wstring FolderOf(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring folder(path, length);
    return folder.substr(0, folder.find_last_of(L'\\'));
}

// ---------------------------------------------------------------------------------------------------------------
// Pictures: PFM in and out, rows top to bottom in memory (PFM stores them bottom to top).

struct Picture
{
    unsigned width = 0, height = 0;
    std::vector<float> rgb; // 3 per pixel
};

bool ReadPfm(const std::wstring& path, Picture* picture)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return false;
    char kind[3] = {};
    unsigned width = 0, height = 0;
    float scale = 0.0f;
    const bool header = fscanf_s(f, "%2s %u %u %f", kind, unsigned(sizeof kind), &width, &height, &scale) == 4 &&
                        (std::strcmp(kind, "PF") == 0 || std::strcmp(kind, "Pf") == 0) && width != 0 && height != 0;
    if (!header || std::fgetc(f) == EOF)
    {
        std::fclose(f);
        return false;
    }
    const unsigned channels = kind[1] == 'F' ? 3 : 1;
    std::vector<float> raw(size_t(width) * height * channels);
    const bool read = std::fread(raw.data(), sizeof(float), raw.size(), f) == raw.size();
    std::fclose(f);
    if (!read)
        return false;
    if (scale > 0.0f) // big-endian
    {
        for (float& v : raw)
        {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            bits = _byteswap_ulong(bits);
            std::memcpy(&v, &bits, 4);
        }
    }
    picture->width = width;
    picture->height = height;
    picture->rgb.resize(size_t(width) * height * 3);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            for (unsigned c = 0; c < 3; ++c)
                picture->rgb[(size_t(y) * width + x) * 3 + c] =
                    raw[(size_t(height - 1 - y) * width + x) * channels + (channels == 3 ? c : 0)];
    return true;
}

bool WritePfm(const std::wstring& path, const Picture& picture)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr)
        return false;
    std::fprintf(f, "PF\n%u %u\n-1.0\n", picture.width, picture.height);
    bool ok = true;
    for (unsigned y = picture.height; y-- > 0;)
        ok = ok && std::fwrite(&picture.rgb[size_t(y) * picture.width * 3], sizeof(float), size_t(picture.width) * 3,
                               f) == size_t(picture.width) * 3;
    return std::fclose(f) == 0 && ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Half floats.

uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u)
        return uint16_t(sign | 0x7E00u); // NaN stays NaN
    // Round to nearest even through the float's own rounding: add, then truncate.
    const int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
    {
        if (exponent < -10)
            return uint16_t(sign);
        mantissa |= 0x800000u;
        const unsigned shift = unsigned(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rest > mid || (rest == mid && (half & 1u) != 0))
            ++half;
        return uint16_t(sign | half);
    }
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0))
        ++half; // may carry into the exponent, which is right (up to infinity)
    return uint16_t(sign | half);
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
            Say("adapter: %s", Utf8(desc.Description).c_str());
            break;
        }
        adapter->Release();
    }
    if (g.adapter == nullptr ||
        FAILED(D3D12CreateDevice(g.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device))))
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

// Runs what is recorded and waits for it; the list comes back open. False on failure.
bool Submit(const char* what)
{
    if (FAILED(g.list->Close()))
    {
        Say("%s: closing the command list failed", what);
        return false;
    }
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    if (g.fence->GetCompletedValue() < g.fenceValue)
    {
        g.fence->SetEventOnCompletion(g.fenceValue, g.event);
        if (WaitForSingleObject(g.event, 15000) != WAIT_OBJECT_0)
        {
            Say("%s: the GPU did not finish within 15 s", what);
            return false;
        }
    }
    const HRESULT removed = g.device->GetDeviceRemovedReason();
    if (removed != S_OK)
    {
        Say("%s: device removed, reason 0x%08lX", what, static_cast<unsigned long>(removed));
        return false;
    }
    g.allocator->Reset();
    g.list->Reset(g.allocator, nullptr);
    return true;
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
    return SUCCEEDED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                       IID_PPV_ARGS(&resource)))
               ? resource
               : nullptr;
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
    return SUCCEEDED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                       IID_PPV_ARGS(&resource)))
               ? resource
               : nullptr;
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

// A staging buffer for one texture's rows (upload or readback), and where they lie in it.
struct Staging
{
    ID3D12Resource* buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
};

bool MakeStaging(ID3D12Resource* texture, D3D12_HEAP_TYPE type, Staging* s)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &s->footprint, nullptr, nullptr, &total);
    s->buffer = MakeBuffer(type, total,
                           type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                          : D3D12_RESOURCE_STATE_COPY_DEST);
    return s->buffer != nullptr;
}

void CopyBetween(ID3D12Resource* texture, const Staging& s, bool toTexture)
{
    D3D12_TEXTURE_COPY_LOCATION placed = {};
    placed.pResource = s.buffer;
    placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    placed.PlacedFootprint = s.footprint;
    D3D12_TEXTURE_COPY_LOCATION whole = {};
    whole.pResource = texture;
    whole.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (toTexture)
        g.list->CopyTextureRegion(&whole, 0, 0, 0, &placed, nullptr);
    else
        g.list->CopyTextureRegion(&placed, 0, 0, 0, &whole, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// The frame's textures, in the format the Output has: RGBA16F (HDR) or RGBA8 UNORM (SDR).

bool g_sdr = false;

void FillRows(const Picture& p, const Staging& s)
{
    uint8_t* mapped = nullptr;
    s.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (unsigned y = 0; y < p.height; ++y)
    {
        uint8_t* row = mapped + s.footprint.Offset + UINT64(y) * s.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < p.width; ++x)
        {
            const float* v = &p.rgb[(size_t(y) * p.width + x) * 3];
            if (g_sdr)
            {
                for (unsigned c = 0; c < 3; ++c)
                {
                    const float e = v[c] > 0.0f ? (v[c] < 1.0f ? v[c] : 1.0f) : 0.0f;
                    row[x * 4 + c] = uint8_t(std::lround(e * 255.0f));
                }
                row[x * 4 + 3] = 255;
            }
            else
            {
                auto* h = reinterpret_cast<uint16_t*>(row) + x * 4;
                h[0] = FloatToHalf(v[0]);
                h[1] = FloatToHalf(v[1]);
                h[2] = FloatToHalf(v[2]);
                h[3] = FloatToHalf(1.0f);
            }
        }
    }
    s.buffer->Unmap(0, nullptr);
}

void ReadRows(const Staging& s, Picture* p)
{
    uint8_t* mapped = nullptr;
    s.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (unsigned y = 0; y < p->height; ++y)
    {
        const uint8_t* row = mapped + s.footprint.Offset + UINT64(y) * s.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < p->width; ++x)
            for (unsigned c = 0; c < 3; ++c)
                p->rgb[(size_t(y) * p->width + x) * 3 + c] =
                    g_sdr ? float(row[x * 4 + c]) / 255.0f
                          : HalfToFloat(reinterpret_cast<const uint16_t*>(row)[x * 4 + c]);
    }
    D3D12_RANGE nothing = { 0, 0 };
    s.buffer->Unmap(0, &nothing);
}

// ---------------------------------------------------------------------------------------------------------------
// The core and the options.

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
using AllocateParameters = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using CreateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                 NVSDK_NGX_Handle**);
using EvaluateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                   NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeature = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);
using Shutdown1 = NVSDK_NGX_Result(__cdecl*)(ID3D12Device*);

unsigned g_ngxLines = 0;

void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature feature)
{
    if (++g_ngxLines > 40 || message == nullptr)
        return;
    size_t length = std::strlen(message);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        --length;
    Say("    ngx[f%d] %.*s", int(feature), int(length), message);
}

struct Shown
{
    std::wstring path;
    unsigned frames = 0; // 0: --frames
};

struct Options
{
    std::wstring out;
    std::wstring snippets;
    std::vector<Shown> pictures;
    std::vector<std::string> sets;
    unsigned frames = 90;
    bool sdr = false;
    bool cuts = false;
    bool haveExposure = false;
    float exposure = 1.0f;
    bool havePreExposure = false;
    float preExposure = 1.0f;
    double fps = 0.0;
    std::string capture = "last";
};

bool ParseOptions(int argc, wchar_t** argv, Options* o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--out" && hasValue)
            o->out = argv[++i];
        else if (arg == L"--snippets" && hasValue)
            o->snippets = argv[++i];
        else if (arg == L"--frames" && hasValue)
            o->frames = unsigned(wcstoul(argv[++i], nullptr, 10));
        else if (arg == L"--sdr")
            o->sdr = true;
        else if (arg == L"--cuts")
            o->cuts = true;
        else if (arg == L"--exposure" && hasValue)
        {
            o->haveExposure = true;
            o->exposure = float(_wtof(argv[++i]));
        }
        else if (arg == L"--pre-exposure" && hasValue)
        {
            o->havePreExposure = true;
            o->preExposure = float(_wtof(argv[++i]));
        }
        else if (arg == L"--fps" && hasValue)
            o->fps = _wtof(argv[++i]);
        else if (arg == L"--capture" && hasValue)
            o->capture = Utf8(argv[++i]);
        else if (arg == L"--set" && hasValue)
            o->sets.push_back(Utf8(argv[++i]));
        else if (!arg.empty() && arg[0] != L'-')
        {
            Shown shown;
            const size_t star = arg.find_last_of(L'*');
            shown.path = star == std::wstring::npos ? arg : arg.substr(0, star);
            if (star != std::wstring::npos)
                shown.frames = unsigned(wcstoul(arg.c_str() + star + 1, nullptr, 10));
            o->pictures.push_back(shown);
        }
        else
            return false;
    }
    return !o->out.empty() && !o->pictures.empty() && o->frames != 0;
}

bool WriteIni(const std::wstring& path, const std::vector<std::string>& sets)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr)
        return false;
    std::fputs("; written by nrhost for this run; the next run replaces it\nBadge=0\nStatsLog=0\n", f);
    for (const std::string& line : sets)
        std::fprintf(f, "%s\n", line.c_str());
    return std::fclose(f) == 0;
}

// The frames to write: "last" (each picture's last), "all", or a list of frame numbers.
std::set<unsigned> Captures(const std::string& spec, const std::vector<unsigned>& lastOfEach, unsigned total)
{
    std::set<unsigned> frames;
    if (spec == "last")
        frames.insert(lastOfEach.begin(), lastOfEach.end());
    else if (spec == "all")
        for (unsigned i = 0; i < total; ++i)
            frames.insert(i);
    else
    {
        const char* p = spec.c_str();
        while (*p != '\0')
        {
            char* end = nullptr;
            const unsigned long n = std::strtoul(p, &end, 10);
            if (end == p)
                break;
            frames.insert(unsigned(n));
            p = *end == ',' ? end + 1 : end;
        }
    }
    return frames;
}

constexpr int kFaulted = -1;

// A shutdown call under a structured exception handler (its own function: SEH cannot share a frame with objects
// that need unwinding).
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

// One SR feature: its parameter block, its Output and the readback for it.
struct Sr
{
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    ID3D12Resource* output = nullptr;
    Staging readback;
};
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options o;
    if (!ParseOptions(argc, argv, &o))
        return Fail("usage: nrhost --out <folder> [--snippets <folder>] [--sdr] [--exposure X] [--pre-exposure X]\n"
                    "              [--fps F] [--cuts] [--frames N] [--capture last|all|I,J,...] [--set Key=Value]...\n"
                    "              <picture.pfm>[*frames]...");
    g_sdr = o.sdr;
    const std::wstring exeFolder = FolderOf(nullptr);
    const HMODULE ours = GetModuleHandleW(L"dxgi.dll");
    if (ours == nullptr || _wcsicmp(FolderOf(ours).c_str(), exeFolder.c_str()) != 0)
        return Fail("dxgi.dll was not loaded from beside this exe: build the solution first");
    if (!FileExists(exeFolder + L"\\nvngx_dlssnr.dll"))
        return Fail("no nvngx_dlssnr.dll beside this exe: copy the model there (tools/nrloop.py does)");
    if (o.snippets.empty())
        o.snippets = exeFolder + L"\\snippets";
    if (!FileExists(o.snippets + L"\\nvngx_dlss.dll"))
        return Fail("no nvngx_dlss.dll in %s: give --snippets, or run tools/nrloop.py prepare", Utf8(o.snippets).c_str());
    if (!WriteIni(exeFolder + L"\\dlssnr.ini", o.sets))
        return Fail("could not write dlssnr.ini beside the exe");
    CreateDirectoryW(o.out.c_str(), nullptr);

    // The pictures and the frame plan.
    std::vector<Picture> pictures(o.pictures.size());
    std::vector<unsigned> firstOf, lastOf;
    unsigned total = 0;
    for (size_t i = 0; i < o.pictures.size(); ++i)
    {
        if (!ReadPfm(o.pictures[i].path, &pictures[i]))
            return Fail("could not read %s as a PFM", Utf8(o.pictures[i].path).c_str());
        if (pictures[i].width != pictures[0].width || pictures[i].height != pictures[0].height)
            return Fail("%s is not the size of the first picture", Utf8(o.pictures[i].path).c_str());
        firstOf.push_back(total);
        total += o.pictures[i].frames != 0 ? o.pictures[i].frames : o.frames;
        lastOf.push_back(total - 1);
    }
    const unsigned w = pictures[0].width, h = pictures[0].height;
    const std::set<unsigned> captures = Captures(o.capture, lastOf, total);
    Say("frames: %u in all, %ux%u %s, %zu pictures, %zu captured%s", total, w, h, o.sdr ? "SDR RGBA8" : "HDR RGBA16F",
        pictures.size(), captures.size(), o.fps > 0.0 ? "" : ", unpaced");

    if (!CreateGpu())
        return Fail("no D3D12 device on an NVIDIA adapter");
    const std::wstring corePath = FindCore();
    const HMODULE core = corePath.empty() ? nullptr : LoadLibraryW(corePath.c_str());
    if (core == nullptr)
        return Fail("the driver's _nvngx.dll was not found or did not load");
    const auto init = Proc<CoreInitExt>(core, "NVSDK_NGX_D3D12_Init_Ext");
    const auto allocate = Proc<AllocateParameters>(core, "NVSDK_NGX_D3D12_AllocateParameters");
    const auto create = Proc<CreateFeature>(core, "NVSDK_NGX_D3D12_CreateFeature");
    const auto evaluate = Proc<EvaluateFeature>(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    const auto release = Proc<ReleaseFeature>(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    const auto shutdown = Proc<Shutdown1>(core, "NVSDK_NGX_D3D12_Shutdown1");
    if (init == nullptr || allocate == nullptr || create == nullptr || evaluate == nullptr || release == nullptr ||
        shutdown == nullptr)
        return Fail("the core lacks an export this needs");

    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring data = std::wstring(local) + L"\\Banana-Zero\\nrhost";
    CreateDirectoryW((std::wstring(local) + L"\\Banana-Zero").c_str(), nullptr);
    CreateDirectoryW(data.c_str(), nullptr);
    const wchar_t* paths[] = { o.snippets.c_str() };
    static NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &OnNgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result result = init(0x42414E41ull, data.c_str(), g.device, NVSDK_NGX_Version_API, &info);
    if (result != NVSDK_NGX_Result_Success)
        return Fail("core Init_Ext -> 0x%08X", unsigned(result));
    Say("core: %s, DLSS from %s", Utf8(corePath).c_str(), Utf8(o.snippets).c_str());

    // The textures.
    const DXGI_FORMAT format = o.sdr ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* colour = MakeTexture(format, w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* depth = MakeTexture(DXGI_FORMAT_R32_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motion = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* exposure = o.haveExposure ? MakeTexture(DXGI_FORMAT_R32_FLOAT, 1, 1, D3D12_RESOURCE_FLAG_NONE,
                                                            D3D12_RESOURCE_STATE_COPY_DEST)
                                              : nullptr;
    Staging upload, depthUpload, motionUpload, exposureUpload;
    if (colour == nullptr || depth == nullptr || motion == nullptr || (o.haveExposure && exposure == nullptr) ||
        !MakeStaging(colour, D3D12_HEAP_TYPE_UPLOAD, &upload) ||
        !MakeStaging(depth, D3D12_HEAP_TYPE_UPLOAD, &depthUpload) ||
        !MakeStaging(motion, D3D12_HEAP_TYPE_UPLOAD, &motionUpload) ||
        (exposure != nullptr && !MakeStaging(exposure, D3D12_HEAP_TYPE_UPLOAD, &exposureUpload)))
        return Fail("could not create the textures");
    // Depth: a plane at 0.5 (not inverted); motion: none. The scene holds still; only its pictures change.
    auto fill = [](const Staging& s, unsigned rows, unsigned rowBytes, auto value) {
        uint8_t* mapped = nullptr;
        s.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (unsigned y = 0; y < rows; ++y)
        {
            auto* row = reinterpret_cast<decltype(value)*>(mapped + s.footprint.Offset +
                                                           UINT64(y) * s.footprint.Footprint.RowPitch);
            for (unsigned x = 0; x < rowBytes / sizeof value; ++x)
                row[x] = value;
        }
        s.buffer->Unmap(0, nullptr);
    };
    fill(depthUpload, h, w * 4, 0.5f);
    fill(motionUpload, h, w * 4, uint32_t(0));
    CopyBetween(depth, depthUpload, true);
    CopyBetween(motion, motionUpload, true);
    Barrier(depth, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    Barrier(motion, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    if (exposure != nullptr)
    {
        fill(exposureUpload, 1, 4, o.exposure);
        CopyBetween(exposure, exposureUpload, true);
        Barrier(exposure, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    }
    if (!Submit("guides"))
        return Fail("could not upload the guides");

    // The two SR features: the first is the one dxgi.dll follows.
    Sr sr[2];
    const int flags = (o.sdr ? 0 : NVSDK_NGX_DLSS_Feature_Flags_IsHDR) |
                      (o.haveExposure ? 0 : NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
    for (Sr& s : sr)
    {
        s.output = MakeTexture(format, w, h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (s.output == nullptr || !MakeStaging(s.output, D3D12_HEAP_TYPE_READBACK, &s.readback) ||
            allocate(&s.params) != NVSDK_NGX_Result_Success)
            return Fail("could not make an SR feature's output or parameters");
        NVSDK_NGX_Parameter* p = s.params;
        p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_Width, w);
        p->Set(NVSDK_NGX_Parameter_Height, h);
        p->Set(NVSDK_NGX_Parameter_OutWidth, w);
        p->Set(NVSDK_NGX_Parameter_OutHeight, h);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(NVSDK_NGX_PerfQuality_Value_DLAA));
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
        result = create(g.list, NVSDK_NGX_Feature_SuperSampling, p, &s.handle);
        if (result != NVSDK_NGX_Result_Success || s.handle == nullptr)
            return Fail("SR CreateFeature -> 0x%08X", unsigned(result));
        if (!Submit("create"))
            return Fail("the SR creation did not run");
    }
    Say("SR: two features at DLAA %ux%u, flags 0x%X", w, h, unsigned(flags));

    // The frames.
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    unsigned failures = 0, written = 0;
    size_t shown = size_t(-1);
    Picture out[2] = { { w, h, std::vector<float>(size_t(w) * h * 3) }, { w, h, std::vector<float>(size_t(w) * h * 3) } };
    for (unsigned frame = 0, pic = 0; frame < total; ++frame)
    {
        while (pic + 1 < pictures.size() && frame >= firstOf[pic + 1])
            ++pic;
        const bool newPicture = shown != pic;
        if (newPicture)
        {
            FillRows(pictures[pic], upload);
            if (frame != 0)
                Barrier(colour, srv, D3D12_RESOURCE_STATE_COPY_DEST);
            CopyBetween(colour, upload, true);
            Barrier(colour, D3D12_RESOURCE_STATE_COPY_DEST, srv);
            shown = pic;
        }
        const unsigned reset = frame == 0 || (o.cuts && newPicture) ? 1u : 0u;
        for (Sr& s : sr)
        {
            NVSDK_NGX_Parameter* p = s.params;
            p->Set(NVSDK_NGX_Parameter_Color, colour);
            p->Set(NVSDK_NGX_Parameter_Output, s.output);
            p->Set(NVSDK_NGX_Parameter_Depth, depth);
            p->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
            p->Set(NVSDK_NGX_Parameter_Reset, reset);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, w);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, h);
            if (o.havePreExposure)
                p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, o.preExposure);
            if (exposure != nullptr)
                p->Set(NVSDK_NGX_Parameter_ExposureTexture, exposure);
            result = evaluate(g.list, s.handle, p, nullptr);
            if (result != NVSDK_NGX_Result_Success)
            {
                if (++failures <= 5)
                    Say("frame %u: SR EvaluateFeature -> 0x%08X", frame, unsigned(result));
            }
        }
        const bool capture = captures.count(frame) != 0;
        if (capture)
            for (Sr& s : sr)
            {
                Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                CopyBetween(s.output, s.readback, false);
                Barrier(s.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        if (!Submit("frame"))
            return Fail("frame %u did not run", frame);
        if (capture)
        {
            const char* const kinds[2] = { "nr", "dlss" };
            for (unsigned i = 0; i < 2; ++i)
            {
                ReadRows(sr[i].readback, &out[i]);
                wchar_t name[64];
                swprintf_s(name, L"\\%04u.%hs.pfm", frame, kinds[i]);
                if (!WritePfm(o.out + name, out[i]))
                    return Fail("could not write %s", Utf8(o.out + name).c_str());
            }
            ++written;
        }
        if (o.fps > 0.0)
        {
            const double due = double(frame + 1) / o.fps;
            for (;;)
            {
                QueryPerformanceCounter(&now);
                const double left = due - double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
                if (left <= 0.0)
                    break;
                Sleep(left > 0.002 ? DWORD(left * 1000.0) - 1 : 0);
            }
        }
    }
    QueryPerformanceCounter(&now);
    Say("ran %u frames in %.1f s, %u captured, %u evaluations failed", total,
        double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart), written, failures);

    // As a game ends: the features released, then the core shut down (dxgi.dll shuts the model down first).
    for (Sr& s : sr)
        if (Guarded(release, s.handle) != int(NVSDK_NGX_Result_Success))
            Say("SR ReleaseFeature failed");
    const int down = Guarded(shutdown, g.device);
    Say("core Shutdown1 -> %s", down == kFaulted ? "faulted" : down == int(NVSDK_NGX_Result_Success) ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
