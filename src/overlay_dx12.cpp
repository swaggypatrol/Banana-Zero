// The overlay: see overlay_dx12.h.
//
// How the frame is reached. Our dxgi.dll only forwards the game's DXGI calls, so the swap chain is created out of
// our sight. When the menu first opens, a swap chain of our own (on a hidden window, with a queue of our own on the
// game's device) shows us the class's virtual function table, which the game's chain shares, and the place inside
// the object where a chain keeps its command queue. The table's Present and Present1 entries point at us while the
// menu is open and are put back when it closes. Every Present of the game's chain then draws the menu: Dear ImGui
// into a small RGBA8 picture of ours, then that picture over the back buffer in the buffer's own encoding (8-bit as
// it is, FP16 as scRGB, 10-bit under an HDR display as PQ), on the queue the chain presents from, so that it lands
// after everything the game drew. Nothing of the chain's is kept between two presents (no buffer, no view), so the
// game's ResizeBuffers needs nothing from us: the next present sees the new size and format.

#include "overlay_dx12.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "log.h"
#include "menu.h"
#include "menu_input.h"
#include "menu_style.h"
#include "settings.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
constexpr unsigned kPresentIndex = 8;   // IDXGISwapChain::Present
constexpr unsigned kPresent1Index = 22; // IDXGISwapChain1::Present1
constexpr unsigned kFrames = 6;         // command allocators in flight
constexpr unsigned kSrvOffscreen = 0;   // our heap: the menu picture
constexpr unsigned kSrvPreview = 1;     // the preview picture
constexpr unsigned kSrvImGui = 2;       // from here: ImGui's textures (the font atlas)
constexpr unsigned kSrvCount = 8;
constexpr float kBaseFontSize = 20.0f; // at 1080 lines; scaled with the frame's height
constexpr double kOutputCheckSeconds = 2.0;

// Where a module's image lies, for "is this the real dxgi.dll's".
struct Image
{
    const char* base;
    size_t size;
};

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* SerializeRootSignatureFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                  ID3DBlob**, ID3DBlob**);

enum ColourMode : unsigned
{
    kModeSdr = 0,   // the buffer holds display-encoded values: the menu goes in as it is
    kModeScRgb = 1, // FP16: linear, 1.0 = 80 nits under an HDR display, SDR white otherwise
    kModePq = 2,    // 10-bit under an HDR display: PQ, Rec. 2020
};

// The composite: the menu picture (premultiplied, sRGB-encoded) over the back buffer, in the buffer's encoding.
const char kCompositeHlsl[] = R"(
Texture2D<float4> gMenu : register(t0);
SamplerState gPoint : register(s0);
cbuffer C : register(b0) { uint mode; float nits; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID)
{
    V v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    v.uv = uv;
    return v;
}
float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Pq(float3 n)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float3 Rec709To2020(float3 c)
{
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)), dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}
float4 PSMain(V v) : SV_Target
{
    float4 m = gMenu.Sample(gPoint, v.uv);
    if (m.a <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float3 c = m.rgb / m.a;
    float3 o;
    if (mode == 0)
        o = c;
    else if (mode == 1)
        o = SrgbToLinear(c) * (nits / 80.0);
    else
        o = Pq(Rec709To2020(SrgbToLinear(c)) * nits);
    return float4(o * m.a, m.a);
}
)";

struct Frame
{
    ID3D12CommandAllocator* allocator = nullptr;
    uint64_t fenceValue = 0; // our fence's value once this frame's list is done; 0 = never used
};

struct Overlay
{
    std::atomic<bool> open { false }; // the entries point at us and the menu is to be drawn
    const char* state = "not opened";
    const char* failure = nullptr; // set once: the overlay gave up for this process

    ID3D12Device* device = nullptr;

    // The table, learned from our own chain.
    bool probed = false;
    void** vtable = nullptr;
    void* realPresent = nullptr;
    void* realPresent1 = nullptr;
    bool patched = false;
    bool leftPatched = false; // another hook came after ours: our entries stay, passing through
    bool queueOffsetKnown = false;
    size_t queueOffset = 0;
    ID3D12CommandQueue* uploadQueue = nullptr; // ours: ImGui's texture uploads wait on it, never on the game's

    // The game's chain, as adopted. No reference is held on the chain: it is known by its address. When it is a
    // wrapper, the real chain inside it is what is drawn on (referenced then).
    IDXGISwapChain* chain = nullptr;
    IDXGISwapChain* native = nullptr;
    bool nativeOwned = false;
    Image dxgi = { nullptr, 0 };         // the real dxgi.dll's image, from the probe
    ID3D12CommandQueue* queue = nullptr; // the chain's, referenced
    HWND window = nullptr;
    const void* refusedChain = nullptr; // a chain we could not draw on, said once
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool displayHdr = false;
    double outputCheckedAt = -1.0e9;
    unsigned mode = kModeSdr;
    float nits = 80.0f;
    bool modeSaid = false; // the log has the first choice too, not only the changes
    uint64_t frames = 0;   // frames drawn
    uint64_t behind = 0;   // presents that went without the menu: the queue had not finished its last use

    // GPU objects of ours.
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT srvSize = 0;
    UINT rtvSize = 0;
    bool srvUsed[kSrvCount] = {};
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    DXGI_FORMAT pipelineFormat = DXGI_FORMAT_UNKNOWN;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12Resource* offscreen = nullptr; // R8G8B8A8_UNORM, width x height, rests in PIXEL_SHADER_RESOURCE
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    Frame frame[kFrames];
    unsigned frameIndex = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* previewPicture = nullptr; // not referenced: the NR pass owns it; the view is remade on change
    unsigned previewGeneration = 0;

    // ImGui.
    bool imgui = false;
    bool systemFont = false; // Segoe UI found
    float styleScale = 0.0f;
    MenuTheme styleTheme = MenuTheme::Dark;
};

Overlay g;

template <typename T> void Release(T** object)
{
    if (*object != nullptr)
    {
        (*object)->Release();
        *object = nullptr;
    }
}

bool Readable(const void* address, size_t bytes)
{
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof info) != sizeof info || info.State != MEM_COMMIT)
        return false;
    const DWORD protect = info.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE || (info.Protect & PAGE_GUARD) != 0)
        return false;
    return uintptr_t(address) + bytes <= uintptr_t(info.BaseAddress) + info.RegionSize;
}

bool InImage(const void* address)
{
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof info) == sizeof info && info.State == MEM_COMMIT &&
           info.Type == MEM_IMAGE;
}

Image ModuleImage(HMODULE module)
{
    Image image = { reinterpret_cast<const char*>(module), 0 };
    if (module == nullptr)
        return image;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
    image.size = nt->OptionalHeader.SizeOfImage;
    return image;
}

bool Within(const Image& image, const void* address)
{
    return image.size != 0 && reinterpret_cast<const char*>(address) >= image.base &&
           reinterpret_cast<const char*>(address) < image.base + image.size;
}

// The file name of the module an address lies in, for the log.
const char* ModuleName(const void* address, char* out, size_t size)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
    {
        snprintf(out, size, "no module");
        return out;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, name != nullptr ? name + 1 : path, -1, out, int(size), nullptr, nullptr);
    return out;
}

// A chain whose table is not dxgi.dll's is another module's wrapper around the real one (Streamline's proxy, an
// overlay's), which the wrapper keeps by its address: the one place in the wrapper holding an object with a table
// of dxgi.dll's that answers to IDXGISwapChain. Returns the chain itself (nothing added) when it is the real one,
// the real one with a reference added when unwrapped, and null when a wrapper hides it.
IDXGISwapChain* NativeChain(IDXGISwapChain* chain, const Image& dxgi, bool* unwrapped)
{
    *unwrapped = false;
    if (dxgi.size == 0 || Within(dxgi, *reinterpret_cast<void* const*>(chain)))
        return chain;
    const char* base = reinterpret_cast<const char*>(chain);
    for (size_t offset = sizeof(void*); offset < 0x200; offset += sizeof(void*))
    {
        if (!Readable(base + offset, sizeof(void*)))
            break;
        const char* candidate = *reinterpret_cast<const char* const*>(base + offset);
        if (candidate == nullptr || candidate == base || !Readable(candidate, sizeof(void*)))
            continue;
        void* const* table = *reinterpret_cast<void* const* const*>(candidate);
        if (!Within(dxgi, table) || !Readable(table, 3 * sizeof(void*)) || !Within(dxgi, table[0]) ||
            !Within(dxgi, table[1]) || !Within(dxgi, table[2]))
            continue;
        IDXGISwapChain* real = nullptr;
        IUnknown* unknown = reinterpret_cast<IUnknown*>(const_cast<char*>(candidate));
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&real))) && real != nullptr)
        {
            *unwrapped = true;
            return real;
        }
    }
    return nullptr;
}

bool WriteEntry(void** entry, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof *entry, PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(entry, value);
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof *entry, old, &ignored);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The probe: our own chain, on a thread of its own, so that the game's render thread never creates a window.

struct Probe
{
    ID3D12Device* device;
    Image dxgi; // the real dxgi.dll's image
    void** vtable;
    void* present;
    void* present1;
    bool queueOffsetKnown;
    size_t queueOffset;
    ID3D12CommandQueue* queue;
    const char* failure;
};

LRESULT CALLBACK ProbeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI ProbeThread(void* argument)
{
    Probe* p = static_cast<Probe*>(argument);
    typedef HRESULT(WINAPI * CreateFactory2Fn)(UINT, REFIID, void**);
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 10 >= MAX_PATH)
    {
        p->failure = "no system directory";
        return 0;
    }
    wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
    const HMODULE dxgi = LoadLibraryW(path); // the real one, loaded already: our exports forward to it
    p->dxgi = ModuleImage(dxgi);
    const auto createFactory =
        dxgi != nullptr ? reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    if (createFactory == nullptr)
    {
        p->failure = "the real dxgi.dll has no CreateDXGIFactory2";
        return 0;
    }

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = &ProbeProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"BananaZeroOverlayProbe";
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        p->failure = "RegisterClassEx failed";
        return 0;
    }
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr,
                                        nullptr, instance, nullptr);
    if (window == nullptr)
    {
        p->failure = "CreateWindowEx failed";
        return 0;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* chain = nullptr;
    if (FAILED(p->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
        p->failure = "CreateCommandQueue failed";
    else if (FAILED(createFactory(0, IID_PPV_ARGS(&factory))))
        p->failure = "CreateDXGIFactory2 failed";
    else
    {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = 64;
        desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HRESULT hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain);
        if (FAILED(hr))
        {
            Log("menu: CreateSwapChainForHwnd on the probe window -> 0x%08X", unsigned(hr));
            p->failure = "CreateSwapChainForHwnd failed";
        }
    }
    if (chain != nullptr)
    {
        // Whose objects these are: a game's process may hand out wrappers even here (TLOU2 does).
        char names[3][64];
        Log("menu: probe: CreateDXGIFactory2 in %s, the factory's table in %s, the chain's table in %s",
            ModuleName(reinterpret_cast<const void*>(createFactory), names[0], sizeof names[0]),
            ModuleName(*reinterpret_cast<void* const*>(factory), names[1], sizeof names[1]),
            ModuleName(*reinterpret_cast<void* const*>(chain), names[2], sizeof names[2]));
        bool unwrapped = false;
        IDXGISwapChain* native = NativeChain(chain, p->dxgi, &unwrapped);
        if (native == nullptr)
            p->failure = "the probe chain is a wrapper and the real chain was not found in it";
        else
        {
            if (unwrapped)
                Log("menu: probe: the chain is a wrapper; the real chain's table is in %s",
                    ModuleName(*reinterpret_cast<void* const*>(native), names[0], sizeof names[0]));
            p->vtable = *reinterpret_cast<void***>(native);
            p->present = p->vtable[kPresentIndex];
            p->present1 = p->vtable[kPresent1Index];
            // Where the chain keeps its queue: the one place in the object holding our queue's address.
            const char* base = reinterpret_cast<const char*>(native);
            for (size_t offset = sizeof(void*); offset < 0x800; offset += sizeof(void*))
            {
                if (!Readable(base + offset, sizeof(void*)))
                    break;
                if (*reinterpret_cast<void* const*>(base + offset) == static_cast<void*>(queue))
                {
                    p->queueOffsetKnown = true;
                    p->queueOffset = offset;
                    break;
                }
            }
            if (unwrapped)
                native->Release();
        }
        chain->Release();
    }
    Release(&factory);
    DestroyWindow(window);
    if (p->failure == nullptr)
        p->queue = queue; // kept: ImGui's uploads
    else
        Release(&queue);
    return 0;
}

// Under the lock. Runs the probe once; false when the overlay cannot be had.
bool ProbeOnce()
{
    if (g.probed)
        return g.failure == nullptr;
    g.probed = true;
    Probe probe = {};
    probe.device = g.device;
    const HANDLE thread = CreateThread(nullptr, 0, &ProbeThread, &probe, 0, nullptr);
    if (thread == nullptr)
    {
        g.failure = "no thread for the probe";
        return false;
    }
    const DWORD waited = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    if (waited != WAIT_OBJECT_0)
    {
        g.failure = "the probe did not finish in 5 s"; // the thread's result is not touched from here on
        return false;
    }
    if (probe.failure != nullptr)
    {
        g.failure = probe.failure;
        return false;
    }
    g.dxgi = probe.dxgi;
    g.vtable = probe.vtable;
    g.realPresent = probe.present;
    g.realPresent1 = probe.present1;
    g.queueOffsetKnown = probe.queueOffsetKnown;
    g.queueOffset = probe.queueOffset;
    g.uploadQueue = probe.queue;
    Log("menu: swap chain table at %p, Present %p, Present1 %p, queue at offset %s%zu", g.vtable, g.realPresent,
        g.realPresent1, g.queueOffsetKnown ? "" : "unknown, would be ", g.queueOffset);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The game's chain.

// The queue the chain presents from: at the place the probe found, checked before it is trusted, else asked of
// the chain itself (which DXGI does not always answer).
ID3D12CommandQueue* FindQueue(IDXGISwapChain* chain)
{
    ID3D12CommandQueue* queue = nullptr;
    if (g.queueOffsetKnown)
    {
        const char* slot = reinterpret_cast<const char*>(chain) + g.queueOffset;
        void* candidate = Readable(slot, sizeof(void*)) ? *reinterpret_cast<void* const*>(slot) : nullptr;
        if (candidate != nullptr && Readable(candidate, sizeof(void*)) &&
            InImage(*reinterpret_cast<void* const*>(candidate)))
        {
            IUnknown* unknown = static_cast<IUnknown*>(candidate);
            if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&queue))))
                queue = nullptr;
        }
    }
    if (queue == nullptr && FAILED(chain->GetDevice(IID_PPV_ARGS(&queue))))
        queue = nullptr;
    if (queue == nullptr)
        return nullptr;
    ID3D12Device* device = nullptr;
    const bool ours = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device == g.device;
    Release(&device);
    if (!ours || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        queue->Release();
        return nullptr;
    }
    return queue;
}

bool OurWindow(HWND window)
{
    if (window == nullptr)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

void DropChain()
{
    MenuInputRelease(); // the subclass stays: the next chain is most often on the same window
    Release(&g.queue);
    if (g.nativeOwned)
        Release(&g.native);
    g.native = nullptr;
    g.nativeOwned = false;
    g.chain = nullptr;
    g.window = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Our GPU objects.

void SrvHandles(unsigned index, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    *cpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += SIZE_T(index) * g.srvSize;
    *gpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += UINT64(index) * g.srvSize;
}

D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(unsigned index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(index) * g.rtvSize;
    return handle;
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    for (unsigned i = kSrvImGui; i < kSrvCount; ++i)
    {
        if (g.srvUsed[i])
            continue;
        g.srvUsed[i] = true;
        SrvHandles(i, cpu, gpu);
        return;
    }
    cpu->ptr = 0; // ImGui asserts on this; it never asks for more than the atlas
    gpu->ptr = 0;
}

void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    const SIZE_T start = g.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr >= start && g.srvSize != 0)
    {
        const SIZE_T index = (cpu.ptr - start) / g.srvSize;
        if (index < kSrvCount)
            g.srvUsed[index] = false;
    }
}

bool CompileShader(const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kCompositeHlsl, sizeof kCompositeHlsl - 1, "overlay", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr))
    {
        Log("menu: the overlay's %s did not compile: 0x%08X %s", entry, unsigned(hr),
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    }
    Release(&errors);
    return SUCCEEDED(hr);
}

bool MakeRootSignature()
{
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize =
        d3d12 != nullptr
            ? reinterpret_cast<SerializeRootSignatureFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
            : nullptr;
    if (serialize == nullptr)
    {
        Log("menu: no D3D12SerializeRootSignature");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 2;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr))
        hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g.rootSignature));
    if (FAILED(hr))
        Log("menu: the overlay's root signature failed: 0x%08X", unsigned(hr));
    Release(&blob);
    Release(&errors);
    return SUCCEEDED(hr);
}

bool MakePipeline(DXGI_FORMAT format)
{
    Release(&g.pipeline);
    g.pipelineFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g.rootSignature;
    desc.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
    desc.PS = { g.ps->GetBufferPointer(), g.ps->GetBufferSize() };
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE; // the picture is premultiplied
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    const HRESULT hr = g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g.pipeline));
    if (FAILED(hr))
    {
        Log("menu: the overlay's pipeline for format %d failed: 0x%08X", int(format), unsigned(hr));
        return false;
    }
    g.pipelineFormat = format;
    return true;
}

// Waits until the GPU is done with everything we submitted. Under the lock.
void WaitForOurWork()
{
    if (g.fence == nullptr || g.fence->GetCompletedValue() >= g.fenceValue)
        return;
    if (SUCCEEDED(g.fence->SetEventOnCompletion(g.fenceValue, g.fenceEvent)))
        WaitForSingleObject(g.fenceEvent, 2000);
}

bool MakeOffscreen(unsigned width, unsigned height)
{
    WaitForOurWork();
    Release(&g.offscreen);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = desc.Format;
    const HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                         IID_PPV_ARGS(&g.offscreen));
    if (FAILED(hr))
    {
        Log("menu: the overlay's %ux%u picture failed: 0x%08X", width, height, unsigned(hr));
        return false;
    }
    g.device->CreateRenderTargetView(g.offscreen, nullptr, RtvHandle(1));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    SrvHandles(kSrvOffscreen, &cpu, &gpu);
    g.device->CreateShaderResourceView(g.offscreen, nullptr, cpu);
    return true;
}

// Once: heaps, shaders, root signature, fence, allocators, the list. False (logged) when something fails.
bool MakeObjects()
{
    if (g.list != nullptr)
        return true;
    D3D12_DESCRIPTOR_HEAP_DESC srv = {};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = kSrvCount;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 2;
    HRESULT hr = g.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g.srvHeap));
    if (SUCCEEDED(hr))
        hr = g.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g.rtvHeap));
    if (SUCCEEDED(hr))
        hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    for (unsigned i = 0; i < kFrames && SUCCEEDED(hr); ++i)
        hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.frame[i].allocator));
    if (SUCCEEDED(hr))
        hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frame[0].allocator, nullptr,
                                         IID_PPV_ARGS(&g.list));
    if (SUCCEEDED(hr))
        hr = g.list->Close();
    if (FAILED(hr))
    {
        Log("menu: the overlay's objects failed: 0x%08X", unsigned(hr));
        Release(&g.list);
        return false;
    }
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.srvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.rtvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.srvUsed[kSrvOffscreen] = true;
    g.srvUsed[kSrvPreview] = true;
    if (g.fenceEvent == nullptr || !CompileShader("VSMain", "vs_5_0", &g.vs) ||
        !CompileShader("PSMain", "ps_5_0", &g.ps) || !MakeRootSignature())
    {
        Release(&g.list);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// ImGui.

// The style for the frame's height and the theme the settings name (menu_style.cpp), redone when either changes.
void ScaleStyle(float scale)
{
    const MenuTheme theme = SettingsCurrent()->menuTheme;
    if (scale == g.styleScale && theme == g.styleTheme)
        return;
    g.styleScale = scale;
    g.styleTheme = theme;
    MenuStyleApply(theme, scale);
}

bool InitImGui(HWND window)
{
    if (g.imgui)
        return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing of ImGui's own goes to disk; dlssnr.ini is ours
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true; // games hide the system cursor: ImGui draws one while the menu is open
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Segoe UI, Windows' own interface font; ImGui's built-in one if it is missing.
    wchar_t fontPath[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(fontPath, MAX_PATH);
    char fontPathA[MAX_PATH * 3] = {};
    if (length != 0 && length + 20 < MAX_PATH)
    {
        wcscat_s(fontPath, MAX_PATH, L"\\Fonts\\segoeui.ttf");
        WideCharToMultiByte(CP_UTF8, 0, fontPath, -1, fontPathA, sizeof fontPathA, nullptr, nullptr);
    }
    if (fontPathA[0] != '\0' && GetFileAttributesW(fontPath) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(fontPathA, kBaseFontSize) != nullptr)
        g.systemFont = true;
    else
    {
        io.Fonts->AddFontDefault();
        Log("menu: segoeui.ttf not found, the built-in font instead");
    }

    if (!ImGui_ImplWin32_Init(window))
    {
        Log("menu: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplDX12_InitInfo info;
    info.Device = g.device;
    info.CommandQueue = g.uploadQueue;
    info.NumFramesInFlight = kFrames;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // the offscreen picture, never the back buffer
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g.srvHeap;
    info.SrvDescriptorAllocFn = &SrvAlloc;
    info.SrvDescriptorFreeFn = &SrvFree;
    if (!ImGui_ImplDX12_Init(&info))
    {
        Log("menu: ImGui_ImplDX12_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    g.imgui = true;
    Log("menu: ImGui %s ready, font %s", IMGUI_VERSION, g.systemFont ? "segoeui.ttf" : "built-in");
    return true;
}

void ShutdownImGui()
{
    if (!g.imgui)
        return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.imgui = false;
    g.styleScale = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// The frame.

// Which encoding the back buffer takes: by its format and the display's mode, or as MenuColour says.
void ChooseMode(IDXGISwapChain* chain, const Settings& s)
{
    const double now = LogClock();
    if (now - g.outputCheckedAt >= kOutputCheckSeconds)
    {
        g.outputCheckedAt = now;
        IDXGIOutput* output = nullptr;
        IDXGIOutput6* output6 = nullptr;
        bool hdr = false;
        if (SUCCEEDED(chain->GetContainingOutput(&output)) && output != nullptr &&
            SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6 != nullptr)
        {
            DXGI_OUTPUT_DESC1 desc;
            if (SUCCEEDED(output6->GetDesc1(&desc)))
                hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
        Release(&output6);
        Release(&output);
        if (hdr != g.displayHdr)
            Log("menu: the display is in %s mode", hdr ? "HDR" : "SDR");
        g.displayHdr = hdr;
    }
    const bool fp16 = g.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool tenBit = g.format == DXGI_FORMAT_R10G10B10A2_UNORM;
    bool hdr = g.displayHdr;
    if (s.menuColour == MenuColour::Sdr)
        hdr = false;
    else if (s.menuColour == MenuColour::Hdr)
        hdr = true;
    unsigned mode = kModeSdr;
    float nits = 80.0f;
    if (fp16)
    {
        mode = kModeScRgb;
        nits = hdr ? s.menuNits : 80.0f;
    }
    else if (tenBit && hdr)
    {
        mode = kModePq;
        nits = s.menuNits;
    }
    if (!g.modeSaid || mode != g.mode || nits != g.nits)
        Log("menu: drawn as %s%s",
            mode == kModePq      ? "PQ"
            : mode == kModeScRgb ? "scRGB"
                                 : "SDR",
            mode == kModeSdr ? ""
            : hdr            ? " at MenuNits"
                             : " at SDR white");
    g.mode = mode;
    g.nits = nits;
    g.modeSaid = true;
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

// Under the menu's lock, with the menu open.
void DrawLocked(IDXGISwapChain* chain)
{
    if (g.failure != nullptr)
        return;
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(chain->GetDesc(&desc)))
        return;

    if (chain != g.chain)
    {
        if (chain == g.refusedChain)
            return;
        DropChain();
        const bool ours = OurWindow(desc.OutputWindow);
        bool unwrapped = false;
        IDXGISwapChain* native = ours ? NativeChain(chain, g.dxgi, &unwrapped) : nullptr;
        ID3D12CommandQueue* queue = native != nullptr ? FindQueue(native) : nullptr;
        if (queue == nullptr)
        {
            if (unwrapped)
                native->Release();
            char module[64];
            g.refusedChain = chain;
            g.state = "no queue for the game's swap chain";
            Log("menu: swap chain %p (window %p, %ux%u, format %d, table in %s): %s, the menu is not drawn on it",
                chain, desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height, int(desc.BufferDesc.Format),
                ModuleName(*reinterpret_cast<void* const*>(chain), module, sizeof module),
                !ours               ? "not this process's window"
                : native == nullptr ? "a wrapper and the real chain was not found in it"
                                    : "its queue was not found");
            return;
        }
        g.chain = chain;
        g.native = native;
        g.nativeOwned = unwrapped;
        g.queue = queue;
        g.window = desc.OutputWindow;
        g.refusedChain = nullptr;
        g.outputCheckedAt = -1.0e9;
        Log("menu: drawing on swap chain %p%s, window %p, %ux%u, format %d, %u buffers", chain,
            unwrapped ? " (the real one inside a wrapper)" : "", g.window, desc.BufferDesc.Width,
            desc.BufferDesc.Height, int(desc.BufferDesc.Format), desc.BufferCount);
        if (!MakeObjects() || !InitImGui(g.window))
        {
            g.failure = "the overlay's objects could not be made";
            g.state = g.failure;
            return;
        }
    }
    MenuInputAttach(g.window); // the keyboard and mouse, for as long as the menu is open (a no-op once attached)
    if (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0)
        return;
    if (g.format != desc.BufferDesc.Format || g.pipeline == nullptr)
    {
        g.format = desc.BufferDesc.Format;
        if (!MakePipeline(g.format))
        {
            g.state = "no pipeline for the back buffer's format";
            return;
        }
    }
    if (g.offscreen == nullptr || g.width != desc.BufferDesc.Width || g.height != desc.BufferDesc.Height)
    {
        g.width = desc.BufferDesc.Width;
        g.height = desc.BufferDesc.Height;
        if (!MakeOffscreen(g.width, g.height))
        {
            g.state = "no picture for the menu";
            return;
        }
    }
    const Settings& s = *SettingsCurrent();
    ChooseMode(g.native, s);

    // This frame's allocator, if the queue is done with the frame that used it kFrames draws ago. Never waited for:
    // where the chain's queue only moves on as frames are presented (Resident Evil Requiem, frame generation and
    // REFramework), a Present held up here held the queue up too, and the game crawled at a few frames a second.
    Frame& frame = g.frame[g.frameIndex % kFrames];
    if (frame.fenceValue != 0 && g.fence->GetCompletedValue() < frame.fenceValue)
    {
        if (g.behind++ == 0)
            Log("menu: the chain's queue had not finished the menu of %u draws ago; such presents go without the "
                "menu (logged once, counted at close)",
                kFrames);
        return;
    }
    if (FAILED(frame.allocator->Reset()) || FAILED(g.list->Reset(frame.allocator, nullptr)))
        return;

    // The ImGui frame.
    ScaleStyle(float(g.height) / 1080.0f);
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(g.width), float(g.height)); // the frame, not the window's client area
    ImGui::NewFrame();
    MenuDraw();
    ImGui::Render();

    // 1. The menu into our picture.
    Barrier(g.offscreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE offscreenRtv = RtvHandle(1);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g.list->OMSetRenderTargets(1, &offscreenRtv, FALSE, nullptr);
    g.list->ClearRenderTargetView(offscreenRtv, clear, 0, nullptr);
    g.list->SetDescriptorHeaps(1, &g.srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g.list);
    Barrier(g.offscreen, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 2. The picture over the back buffer.
    IDXGISwapChain3* chain3 = nullptr;
    ID3D12Resource* back = nullptr;
    UINT index = 0;
    if (SUCCEEDED(g.native->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3 != nullptr)
        index = chain3->GetCurrentBackBufferIndex();
    Release(&chain3);
    if (FAILED(g.native->GetBuffer(index, IID_PPV_ARGS(&back))) || back == nullptr)
    {
        g.list->Close();
        return;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE backRtv = RtvHandle(0);
    g.device->CreateRenderTargetView(back, nullptr, backRtv);
    Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.list->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, float(g.width), float(g.height), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, LONG(g.width), LONG(g.height) };
    g.list->RSSetViewports(1, &viewport);
    g.list->RSSetScissorRects(1, &scissor);
    g.list->SetGraphicsRootSignature(g.rootSignature);
    g.list->SetPipelineState(g.pipeline);
    g.list->SetDescriptorHeaps(1, &g.srvHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    SrvHandles(kSrvOffscreen, &cpu, &gpu);
    g.list->SetGraphicsRootDescriptorTable(0, gpu);
    uint32_t constants[2];
    constants[0] = g.mode;
    memcpy(&constants[1], &g.nits, sizeof constants[1]);
    g.list->SetGraphicsRoot32BitConstants(1, 2, constants, 0);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    back->Release(); // before the game's ResizeBuffers could ask: the list holds no reference

    if (FAILED(g.list->Close()))
        return;
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    frame.fenceValue = ++g.fenceValue;
    g.queue->Signal(g.fence, g.fenceValue);
    ++g.frameIndex;
    ++g.frames;
    g.state = "drawing";
}

void Draw(IDXGISwapChain* chain, UINT flags)
{
    if ((flags & DXGI_PRESENT_TEST) != 0 || !g.open.load(std::memory_order_acquire))
        return;
    MenuLock();
    if (g.open.load(std::memory_order_relaxed))
        DrawLocked(chain);
    MenuUnlock();
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT sync, UINT flags)
{
    Draw(chain, flags);
    return reinterpret_cast<PresentFn>(g.realPresent)(chain, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* chain, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters)
{
    Draw(chain, flags);
    return reinterpret_cast<Present1Fn>(g.realPresent1)(chain, sync, flags, parameters);
}

// Under the lock.
void Patch()
{
    if (g.patched)
        return;
    void** present = g.vtable + kPresentIndex;
    void** present1 = g.vtable + kPresent1Index;
    if (*present != reinterpret_cast<void*>(&HookPresent) &&
        !WriteEntry(present, reinterpret_cast<void*>(&HookPresent)))
    {
        g.failure = "the table's Present entry could not be written";
        return;
    }
    if (*present1 != reinterpret_cast<void*>(&HookPresent1) &&
        !WriteEntry(present1, reinterpret_cast<void*>(&HookPresent1)))
    {
        WriteEntry(present, g.realPresent);
        g.failure = "the table's Present1 entry could not be written";
        return;
    }
    g.patched = true;
}

// Under the lock. An entry that no longer points at us was taken over by a later hook, which now forwards to us:
// it stays, and so does ours, passing through.
void Unpatch()
{
    if (!g.patched)
        return;
    void** present = g.vtable + kPresentIndex;
    void** present1 = g.vtable + kPresent1Index;
    bool left = false;
    if (*present == reinterpret_cast<void*>(&HookPresent))
        WriteEntry(present, g.realPresent);
    else
        left = true;
    if (*present1 == reinterpret_cast<void*>(&HookPresent1))
        WriteEntry(present1, g.realPresent1);
    else
        left = true;
    g.patched = left;
    if (left && !g.leftPatched)
    {
        g.leftPatched = true;
        Log("menu: another hook took the swap chain's Present after ours: our entries stay, passing through");
    }
}
} // namespace

bool OverlayOpen(ID3D12GraphicsCommandList* list)
{
    if (g.device == nullptr && list != nullptr && FAILED(list->GetDevice(IID_PPV_ARGS(&g.device))))
        g.device = nullptr;
    bool ok = false;
    if (g.device == nullptr)
        g.state = "no device yet";
    else if (g.failure != nullptr)
        g.state = g.failure;
    else if (ProbeOnce())
    {
        Patch();
        ok = g.failure == nullptr;
        g.state = ok ? "waiting for the first present" : g.failure;
    }
    else
        g.state = g.failure;
    if (ok)
        g.open.store(true, std::memory_order_release);
    else
        Log("menu: the overlay cannot be drawn: %s", g.state);
    return ok;
}

void OverlayClose()
{
    g.open.store(false, std::memory_order_release);
    MenuInputRelease();
    Unpatch();
    if (g.behind != 0)
        Log("menu: so far drawn on %llu presents; %llu went without it, the chain's queue behind",
            static_cast<unsigned long long>(g.frames), static_cast<unsigned long long>(g.behind));
    if (g.failure == nullptr)
        g.state = "closed";
}

void OverlayRelease()
{
    g.open.store(false, std::memory_order_release);
    MenuInputDetach();
    Unpatch();
    WaitForOurWork();
    ShutdownImGui();
    Release(&g.offscreen);
    Release(&g.pipeline);
    Release(&g.rootSignature);
    Release(&g.vs);
    Release(&g.ps);
    Release(&g.list);
    for (Frame& frame : g.frame)
    {
        Release(&frame.allocator);
        frame.fenceValue = 0;
    }
    Release(&g.fence);
    if (g.fenceEvent != nullptr)
    {
        CloseHandle(g.fenceEvent);
        g.fenceEvent = nullptr;
    }
    Release(&g.srvHeap);
    Release(&g.rtvHeap);
    memset(g.srvUsed, 0, sizeof g.srvUsed);
    DropChain();
    Release(&g.uploadQueue);
    Release(&g.device);
    g.probed = false; // the next open probes again, on whatever device the game has then
    g.vtable = nullptr;
    g.queueOffsetKnown = false;
    g.previewPicture = nullptr;
    g.previewGeneration = 0;
    g.format = DXGI_FORMAT_UNKNOWN;
    g.pipelineFormat = DXGI_FORMAT_UNKNOWN;
    g.width = g.height = 0;
    g.fenceValue = 0;
    g.frameIndex = 0;
    g.modeSaid = false;
    if (g.failure == nullptr)
        g.state = "released";
}

ImTextureID OverlayPreviewTexture(ID3D12Resource* picture, unsigned generation, DXGI_FORMAT format)
{
    if (picture == nullptr || g.srvHeap == nullptr)
        return ImTextureID(0);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    SrvHandles(kSrvPreview, &cpu, &gpu);
    if (picture != g.previewPicture || generation != g.previewGeneration)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(picture, &desc, cpu);
        g.previewPicture = picture;
        g.previewGeneration = generation;
    }
    return ImTextureID(gpu.ptr);
}

bool OverlayFrameSize(unsigned* width, unsigned* height)
{
    if (g.chain == nullptr || g.width == 0 || g.height == 0)
        return false;
    *width = g.width;
    *height = g.height;
    return true;
}

const char* OverlayState() { return g.state; }
