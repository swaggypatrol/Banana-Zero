// ngxtest: checks dxgi.dll's interception of the NGX core against a stand-in core, fakecore\_nvngx.dll, and the
// bridge banana.nvngx.dll against a stand-in model, fakemodel\nvngx_dlssnr.dll, without a game, a driver or a GPU.
// Built beside dxgi.dll.
//
//   build\Release\ngxtest.exe
//
// First, in this process: the bridge. The fake model refuses a caller whose module is not named like nvngx.dll, as
// the real one does; ngxtest calls it straight (refused), then through the bridge (accepted, every argument and
// result passed on unchanged, the bridge's frame on the stack for each call).
// Then each core scenario runs in a child process of its own (a fresh loader and a fresh dlssnr.log):
//   early  dxgi.dll is loaded first and the core after it, the order in a game. Checks that the core's five exports
//          lead to trampolines just above the core that jump into dxgi.dll, that its other exports and the page
//          protections are as before, that every call reaches the core unchanged and every result comes back
//          unchanged, that a DLL named like a DLSS snippet is left alone, and that a core unloaded and loaded again is
//          patched again, and that the NR pass sees the evaluations and passes when they carry no Output texture.
//   late   the core is already loaded when dxgi.dll is: it is patched then, and an address taken before stays the
//          core's own.
// After each, checks what dxgi.dll wrote to dlssnr.log. Prints every check; the exit code is the number that failed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fakecore.h"
#include "fakemodel.h"
#include "nvsdk_ngx.h"

namespace
{
int g_failures = 0;

void Check(bool ok, const char* format, ...)
{
    std::fputs(ok ? "  ok    " : "  FAIL  ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    if (!ok)
        ++g_failures;
}

std::wstring Folder()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder(path, length);
    return folder.substr(0, folder.find_last_of(L'\\'));
}

const std::wstring kDxgi = L"\\dxgi.dll";
const std::wstring kCore = L"\\fakecore\\_nvngx.dll";
const std::wstring kSnippet = L"\\fakecore\\nvngx_dlss.dll";
const char* const kIntercepted[] = { "NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D12_EvaluateFeature",
                                     "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_Shutdown1",
                                     "NVSDK_NGX_D3D12_Shutdown" };

struct Image
{
    const BYTE* base = nullptr;
    size_t size = 0;
    bool Holds(const void* address) const { return address >= base && static_cast<const BYTE*>(address) < base + size; }
};

Image ImageOf(HMODULE module)
{
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return { base, nt->OptionalHeader.SizeOfImage };
}

// The export table entry for `name` in a loaded module.
const DWORD* ExportEntry(HMODULE module, const char* name)
{
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + table->AddressOfNames);
    const auto* indices = reinterpret_cast<const WORD*>(base + table->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + table->AddressOfFunctions);
    for (DWORD i = 0; i < table->NumberOfNames; ++i)
    {
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
            return &functions[indices[i]];
    }
    return nullptr;
}

DWORD Protection(const void* address)
{
    MEMORY_BASIC_INFORMATION region = {};
    VirtualQuery(address, &region, sizeof region);
    return region.Protect;
}

bool Writable(DWORD protection)
{
    return (protection & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

unsigned long long Offset(const void* address, const Image& image)
{
    return static_cast<unsigned long long>(static_cast<const BYTE*>(address) - image.base);
}

// The core's five exports lead to trampolines within 2 GB above it that jump into dxgi.dll; everything else about
// its export table is as built.
void CheckPatched(HMODULE core, HMODULE ours)
{
    const Image coreImage = ImageOf(core);
    const Image ourImage = ImageOf(ours);
    for (const char* name : kIntercepted)
    {
        const auto* address = reinterpret_cast<const BYTE*>(GetProcAddress(core, name));
        const bool inReach =
            address != nullptr && address >= coreImage.base + coreImage.size && address < coreImage.base + 0x7FFF0000;
        int32_t displacement = -1;
        const void* target = nullptr;
        if (inReach)
        {
            std::memcpy(&displacement, address + 2, sizeof displacement);
            std::memcpy(&target, address + 6, sizeof target);
        }
        const bool jump = inReach && address[0] == 0xFF && address[1] == 0x25 && displacement == 0;
        const DWORD protection = inReach ? Protection(address) : 0;
        Check(jump && ourImage.Holds(target) && protection == PAGE_EXECUTE_READ,
              "%s -> core+0x%llX: jmp to dxgi.dll+0x%llX, page protection 0x%lX", name,
              address != nullptr ? Offset(address, coreImage) : 0ull, jump ? Offset(target, ourImage) : 0ull,
              protection);
    }

    const void* init = reinterpret_cast<const void*>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
    Check(coreImage.Holds(init), "NVSDK_NGX_D3D12_Init is left alone: core+0x%llX",
          init != nullptr ? Offset(init, coreImage) : 0ull);
    const DWORD* entry = ExportEntry(core, kIntercepted[0]);
    const DWORD protection = entry != nullptr ? Protection(entry) : 0;
    Check(entry != nullptr && !Writable(protection), "the export table is read-only again (0x%lX)", protection);
}

// A parameter block with the creation parameters of a 4K HDR game rendering at 1080p. Counts reads and writes:
// dxgi.dll may read the game's parameters but never write them.
struct TestParameters : NVSDK_NGX_Parameter
{
    mutable int reads = 0;
    int writes = 0;

    void Set(const char*, unsigned long long) override { ++writes; }
    void Set(const char*, float) override { ++writes; }
    void Set(const char*, double) override { ++writes; }
    void Set(const char*, unsigned int) override { ++writes; }
    void Set(const char*, int) override { ++writes; }
    void Set(const char*, ID3D11Resource*) override { ++writes; }
    void Set(const char*, ID3D12Resource*) override { ++writes; }
    void Set(const char*, void*) override { ++writes; }
    void Reset() override { ++writes; }

    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override
    {
        ++reads;
        const struct
        {
            const char* name;
            unsigned value;
        } known[] = { { NVSDK_NGX_Parameter_Width, 1920 },
                      { NVSDK_NGX_Parameter_Height, 1080 },
                      { NVSDK_NGX_Parameter_OutWidth, 3840 },
                      { NVSDK_NGX_Parameter_OutHeight, 2160 } };
        for (const auto& entry : known)
        {
            if (std::strcmp(name, entry.name) == 0)
            {
                *value = entry.value;
                return NVSDK_NGX_Result_Success;
            }
        }
        return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        ++reads;
        if (std::strcmp(name, NVSDK_NGX_Parameter_PerfQualityValue) == 0)
            *value = 1;
        else if (std::strcmp(name, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) == 0)
            *value = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                     NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        else
            return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return Unsupported(); }
    NVSDK_NGX_Result Get(const char*, float*) const override { return Unsupported(); }
    NVSDK_NGX_Result Get(const char*, double*) const override { return Unsupported(); }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return Unsupported(); }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return Unsupported(); }
    NVSDK_NGX_Result Get(const char*, void**) const override { return Unsupported(); }

  private:
    NVSDK_NGX_Result Unsupported() const
    {
        ++reads;
        return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    }
};

using CreateFeature = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFeature = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFeature = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
using Shutdown1 = decltype(&NVSDK_NGX_D3D12_Shutdown1);

template <typename Function> Function Proc(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// The core's functions as a caller would get them, with GetProcAddress.
struct Core
{
    HMODULE module = nullptr;
    CreateFeature create = nullptr;
    EvaluateFeature evaluate = nullptr;
    ReleaseFeature release = nullptr;
    Shutdown1 shutdown1 = nullptr;
    FakeLastCall lastCall = nullptr;
    FakeSetEvaluateResult setEvaluateResult = nullptr;

    explicit Core(HMODULE m) : module(m)
    {
        create = Proc<CreateFeature>(m, kIntercepted[0]);
        evaluate = Proc<EvaluateFeature>(m, kIntercepted[1]);
        release = Proc<ReleaseFeature>(m, kIntercepted[2]);
        shutdown1 = Proc<Shutdown1>(m, kIntercepted[3]);
        lastCall = Proc<FakeLastCall>(m, "FakeCore_LastCall");
        setEvaluateResult = Proc<FakeSetEvaluateResult>(m, "FakeCore_SetEvaluateResult");
    }
    FakeCall Last() const
    {
        FakeCall call = {};
        lastCall(&call);
        return call;
    }
};

bool Is(const FakeCall& call, const char* function, const void* a0, const void* a1, const void* a2, const void* a3)
{
    return call.function != nullptr && std::strcmp(call.function, function) == 0 && call.args[0] == a0 &&
           call.args[1] == a1 && call.args[2] == a2 && call.args[3] == a3;
}

auto* const kList = reinterpret_cast<ID3D12GraphicsCommandList*>(0x1111);
const auto kCallback = reinterpret_cast<PFN_NVSDK_NGX_ProgressCallback>(0x3333);
const void* const kCallbackArgument = reinterpret_cast<const void*>(0x3333); // kCallback as the fake core records it
constexpr NVSDK_NGX_Result kInvalid = NVSDK_NGX_Result_FAIL_InvalidParameter;

const void* Feature(int feature) { return reinterpret_cast<const void*>(static_cast<intptr_t>(feature)); }

int Early(const std::wstring& folder)
{
    std::puts("Load dxgi.dll, then the core");
    const HMODULE ours = LoadLibraryW((folder + kDxgi).c_str());
    Check(ours != nullptr, "dxgi.dll loaded (%lu)", ours != nullptr ? 0 : GetLastError());
    HMODULE module = LoadLibraryW((folder + kCore).c_str());
    Check(module != nullptr, "fakecore\\_nvngx.dll loaded (%lu)", module != nullptr ? 0 : GetLastError());
    if (ours == nullptr || module == nullptr)
        return g_failures;
    const Image ourImage = ImageOf(ours);

    std::puts("Patch");
    CheckPatched(module, ours);

    std::puts("Calls");
    Core core(module);
    TestParameters params;
    NVSDK_NGX_Handle* sr = nullptr;
    NVSDK_NGX_Result result = core.create(kList, NVSDK_NGX_Feature_SuperSampling, &params, &sr);
    FakeCall call = core.Last();
    Check(result == NVSDK_NGX_Result_Success && sr != nullptr && sr->Id == kFakeFirstId,
          "CreateFeature(SR) -> 0x%08X, handle #%u", unsigned(result), sr != nullptr ? sr->Id : 0);
    Check(Is(call, "CreateFeature", kList, Feature(1), &params, &sr),
          "the core got the same command list, feature, parameters and handle pointer");
    Check(ourImage.Holds(call.caller), "the core was called from dxgi.dll+0x%llX",
          ourImage.Holds(call.caller) ? Offset(call.caller, ourImage) : 0ull);
    Check(params.reads > 0 && params.writes == 0, "dxgi.dll read the SR creation parameters (%d reads) and wrote none",
          params.reads);

    const NVSDK_NGX_Result expected[] = {
        NVSDK_NGX_Result_Success, NVSDK_NGX_Result_Success, NVSDK_NGX_Result_Success, kInvalid, kInvalid,
        NVSDK_NGX_Result_Success
    };
    bool same = true;
    for (size_t i = 0; i < std::size(expected); ++i)
    {
        core.setEvaluateResult(int(expected[i]));
        result = core.evaluate(kList, sr, &params, kCallback);
        same =
            same && result == expected[i] && Is(core.Last(), "EvaluateFeature", kList, sr, &params, kCallbackArgument);
    }
    Check(same,
          "6 EvaluateFeature(SR): the core got every argument, and its results 1 1 1 0x%08X 0x%08X 1 came "
          "back unchanged",
          unsigned(kInvalid), unsigned(kInvalid));

    NVSDK_NGX_Handle* fg = nullptr;
    result = core.create(kList, NVSDK_NGX_Feature_FrameGeneration, nullptr, &fg);
    Check(result == NVSDK_NGX_Result_Success && fg != nullptr, "CreateFeature(FG) -> 0x%08X", unsigned(result));
    same = true;
    for (int i = 0; i < 2; ++i)
        same = same && core.evaluate(kList, fg, nullptr, nullptr) == NVSDK_NGX_Result_Success;
    Check(same, "2 EvaluateFeature(FG) -> 1");

    auto* const untouched = reinterpret_cast<NVSDK_NGX_Handle*>(0x5555);
    NVSDK_NGX_Handle* refused = untouched;
    result = core.create(kList, NVSDK_NGX_Feature(kFakeRefusedFeature), &params, &refused);
    Check(result == kInvalid && refused == untouched, "a refused CreateFeature -> 0x%08X, handle left as it was",
          unsigned(result));

    FakeHandle stranger = { 7 };
    auto* const strangerHandle = reinterpret_cast<NVSDK_NGX_Handle*>(&stranger);
    result = core.evaluate(kList, strangerHandle, nullptr, nullptr);
    Check(result == NVSDK_NGX_Result_Success &&
              Is(core.Last(), "EvaluateFeature", kList, strangerHandle, nullptr, nullptr),
          "EvaluateFeature on a handle dxgi.dll did not see created still reaches the core");

    result = core.release(sr);
    Check(result == NVSDK_NGX_Result_Success && Is(core.Last(), "ReleaseFeature", sr, nullptr, nullptr, nullptr),
          "ReleaseFeature(SR) -> 0x%08X", unsigned(result));
    result = core.release(fg);
    Check(result == NVSDK_NGX_Result_Success, "ReleaseFeature(FG) -> 0x%08X", unsigned(result));
    auto* const device = reinterpret_cast<ID3D12Device*>(0x3333);
    result = core.shutdown1(device);
    Check(result == NVSDK_NGX_Result_Success && Is(core.Last(), "Shutdown1", device, nullptr, nullptr, nullptr) &&
              ourImage.Holds(core.Last().caller),
          "Shutdown1(device) -> 0x%08X, through dxgi.dll", unsigned(result));

    std::puts("A snippet with the same export names");
    const HMODULE snippet = LoadLibraryW((folder + kSnippet).c_str());
    const void* snippetCreate =
        snippet != nullptr ? reinterpret_cast<const void*>(GetProcAddress(snippet, kIntercepted[0])) : nullptr;
    Check(snippet != nullptr && ImageOf(snippet).Holds(snippetCreate),
          "fakecore\\nvngx_dlss.dll is left alone: its CreateFeature is its own");

    std::puts("Unload the core and load it again");
    FreeLibrary(module);
    Check(GetModuleHandleW(L"_nvngx.dll") == nullptr, "the core is unloaded");
    module = LoadLibraryW((folder + kCore).c_str());
    Check(module != nullptr, "and loaded again");
    if (module == nullptr)
        return g_failures;
    CheckPatched(module, ours);
    Core again(module);
    NVSDK_NGX_Handle* sr2 = nullptr;
    result = again.create(kList, NVSDK_NGX_Feature_SuperSampling, &params, &sr2);
    const NVSDK_NGX_Result evaluated = again.evaluate(kList, sr2, &params, kCallback);
    const NVSDK_NGX_Result released = again.release(sr2);
    Check(result == NVSDK_NGX_Result_Success && evaluated == NVSDK_NGX_Result_Success &&
              released == NVSDK_NGX_Result_Success && ourImage.Holds(again.Last().caller),
          "Create, Evaluate, Release on the reloaded core all go through dxgi.dll");

    // Unloaded here rather than left to process exit, so the log has one unload per load whatever the loader
    // reports while the process ends.
    FreeLibrary(module);
    Check(GetModuleHandleW(L"_nvngx.dll") == nullptr, "the reloaded core is unloaded");
    return g_failures;
}

int Late(const std::wstring& folder)
{
    std::puts("Load the core, then dxgi.dll");
    const HMODULE module = LoadLibraryW((folder + kCore).c_str());
    Check(module != nullptr, "fakecore\\_nvngx.dll loaded (%lu)", module != nullptr ? 0 : GetLastError());
    if (module == nullptr)
        return g_failures;
    const Core before(module); // addresses taken before dxgi.dll is loaded
    const HMODULE ours = LoadLibraryW((folder + kDxgi).c_str());
    Check(ours != nullptr, "dxgi.dll loaded (%lu)", ours != nullptr ? 0 : GetLastError());
    if (ours == nullptr)
        return g_failures;

    std::puts("Patch");
    CheckPatched(module, ours);
    Check(ImageOf(module).Holds(reinterpret_cast<const void*>(before.create)),
          "an address taken before still leads straight into the core");

    std::puts("Calls");
    const Core core(module);
    NVSDK_NGX_Handle* sr = nullptr;
    NVSDK_NGX_Handle* rr = nullptr;
    const NVSDK_NGX_Result createdSr = core.create(kList, NVSDK_NGX_Feature_SuperSampling, nullptr, &sr);
    const NVSDK_NGX_Result evaluatedSr = core.evaluate(kList, sr, nullptr, nullptr);
    const NVSDK_NGX_Result createdRr = before.create(kList, NVSDK_NGX_Feature_RayReconstruction, nullptr, &rr);
    const NVSDK_NGX_Result evaluatedRr = core.evaluate(kList, rr, nullptr, nullptr);
    const NVSDK_NGX_Result releasedSr = core.release(sr);
    const NVSDK_NGX_Result releasedRr = core.release(rr);
    Check(createdSr == NVSDK_NGX_Result_Success && evaluatedSr == NVSDK_NGX_Result_Success &&
              createdRr == NVSDK_NGX_Result_Success && evaluatedRr == NVSDK_NGX_Result_Success &&
              releasedSr == NVSDK_NGX_Result_Success && releasedRr == NVSDK_NGX_Result_Success,
          "SR through dxgi.dll, RR created past it: every call returns 1");
    return g_failures;
}

// ---------------------------------------------------------------------------------------------------------------
// The bridge, in this process.

using ModelInitExt = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
using BzLoad = int(__cdecl*)(const wchar_t*);
using BzPopulate = int(__cdecl*)(void*);
using BzCreate = int(__cdecl*)(void*, int, void*, void**);
using BzEvaluate = int(__cdecl*)(void*, const void*, void*, void*);
using BzRelease = int(__cdecl*)(void*);
using BzShutdown = int(__cdecl*)(void*);
using BzBuild = const char*(__cdecl*)();

FakeModelCall ModelLast(HMODULE model)
{
    FakeModelCall call = {};
    Proc<FakeModelLastCall>(model, "FakeModel_LastCall")(&call);
    return call;
}

bool ModelIs(const FakeModelCall& call, const char* function, const void* a0, const void* a1, const void* a2,
             const void* a3)
{
    return call.function != nullptr && std::strcmp(call.function, function) == 0 && call.args[0] == a0 &&
           call.args[1] == a1 && call.args[2] == a2 && call.args[3] == a3;
}

void Bridge(const std::wstring& folder)
{
    const std::wstring modelPath = folder + L"\\fakemodel\\nvngx_dlssnr.dll";
    const HMODULE model = LoadLibraryW(modelPath.c_str());
    Check(model != nullptr, "fakemodel\\nvngx_dlssnr.dll loaded (%lu)", model != nullptr ? 0 : GetLastError());
    if (model == nullptr)
        return;
    int result = Proc<ModelInitExt>(model, "NVSDK_NGX_D3D12_Init_Ext")(1, L"data", nullptr, 0x15, nullptr);
    FakeModelCall call = ModelLast(model);
    Check(result == kFakeModelPlatformError && !call.accepted,
          "called straight from ngxtest.exe, the model answers 0x%08X: its caller check", unsigned(result));

    const HMODULE bridge = LoadLibraryW((folder + L"\\banana.nvngx.dll").c_str());
    Check(bridge != nullptr, "banana.nvngx.dll loaded (%lu)", bridge != nullptr ? 0 : GetLastError());
    if (bridge == nullptr)
        return;
    const Image bridgeImage = ImageOf(bridge);
    const auto load = Proc<BzLoad>(bridge, "bz_load");
    const auto init = Proc<ModelInitExt>(bridge, "bz_init");
    const auto populate = Proc<BzPopulate>(bridge, "bz_populate");
    const auto create = Proc<BzCreate>(bridge, "bz_create");
    const auto evaluate = Proc<BzEvaluate>(bridge, "bz_evaluate");
    const auto release = Proc<BzRelease>(bridge, "bz_release");
    const auto shutdown = Proc<BzShutdown>(bridge, "bz_shutdown");
    const auto build = Proc<BzBuild>(bridge, "bz_build");
    const bool exported = load != nullptr && init != nullptr && populate != nullptr && create != nullptr &&
                          evaluate != nullptr && release != nullptr && shutdown != nullptr && build != nullptr;
    Check(exported, "bz_load, bz_init, bz_populate, bz_create, bz_evaluate, bz_release, bz_shutdown and bz_build "
                    "are exported");
    if (!exported)
        return;
    const char* hash = build();
    Check(hash != nullptr && *hash != '\0', "bz_build() = \"%s\"", hash != nullptr ? hash : "(null)");

    result = init(1, L"data", nullptr, 0x15, nullptr);
    Check(result == int(0xBAD00007), "before bz_load, bz_init answers 0x%08X (NotInitialized)", unsigned(result));
    int found = load(modelPath.c_str());
    Check(found == 63, "bz_load(fakemodel) -> 0x%X: all six entry points found", unsigned(found));
    found = load(modelPath.c_str());
    Check(found == 63, "bz_load again -> 0x%X, a no-op", unsigned(found));

    auto* const device = reinterpret_cast<void*>(0x2222);
    const wchar_t* const data = L"C:\\Banana-Zero\\ngx";
    const auto info = reinterpret_cast<const void*>(0x4444);
    result = init(0x42414E41ull, data, device, 0x15, info);
    call = ModelLast(model);
    Check(result == kFakeModelSuccess && ModelIs(call, "Init_Ext", data, device, info, nullptr) &&
              call.appId == 0x42414E41ull && call.version == 0x15,
          "bz_init(0x42414E41, data dir, device, 0x15, info) -> 0x%08X; the model got every argument",
          unsigned(result));
    Check(call.accepted && bridgeImage.Holds(call.caller),
          "the model saw banana.nvngx.dll+0x%llX as its caller: the bridge's frame stayed on the stack (no tail call)",
          bridgeImage.Holds(call.caller) ? Offset(call.caller, bridgeImage) : 0ull);

    int block = 0;
    result = populate(&block);
    Check(result == kFakeModelSuccess &&
              ModelIs(ModelLast(model), "PopulateParameters_Impl", &block, nullptr, nullptr, nullptr),
          "bz_populate(block) -> 0x%08X with the block", unsigned(result));

    void* handle = nullptr;
    result = create(kList, 18, &block, &handle);
    call = ModelLast(model);
    const unsigned id = handle != nullptr ? static_cast<const FakeModelHandle*>(handle)->Id : 0;
    Check(result == kFakeModelSuccess && id == kFakeModelFirstId &&
              ModelIs(call, "CreateFeature", kList, Feature(18), &block, &handle) && bridgeImage.Holds(call.caller),
          "bz_create(list, 18, block) -> 0x%08X, handle #%u, from the bridge", unsigned(result), id);
    void* refused = nullptr;
    result = create(kList, 1, &block, &refused);
    Check(result == kFakeModelInvalidParameter && refused == nullptr,
          "bz_create(list, 1, block): the model's own refusal 0x%08X comes back unchanged", unsigned(result));

    result = evaluate(kList, handle, &block, nullptr);
    call = ModelLast(model);
    Check(result == kFakeModelSuccess && ModelIs(call, "EvaluateFeature", kList, handle, &block, nullptr) &&
              bridgeImage.Holds(call.caller),
          "bz_evaluate(list, handle, block, null) -> 0x%08X, from the bridge", unsigned(result));
    result = release(handle);
    call = ModelLast(model);
    Check(result == kFakeModelSuccess && ModelIs(call, "ReleaseFeature", handle, nullptr, nullptr, nullptr) &&
              bridgeImage.Holds(call.caller),
          "bz_release(handle) -> 0x%08X, from the bridge", unsigned(result));
    result = shutdown(device);
    call = ModelLast(model);
    Check(result == kFakeModelSuccess && ModelIs(call, "Shutdown1", device, nullptr, nullptr, nullptr) &&
              bridgeImage.Holds(call.caller),
          "bz_shutdown(device) -> 0x%08X, from the bridge", unsigned(result));
    Check(call.calls == 8, "the model was called 8 times in all (%u)", call.calls);
}

std::vector<std::string> ReadLog(const std::wstring& folder)
{
    std::ifstream file((folder + L"\\dlssnr.log").c_str());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line))
    {
        std::printf("  | %s\n", line.c_str());
        lines.push_back(line);
    }
    return lines;
}

std::string Utf8(const std::wstring& text)
{
    std::string out(text.size() * 3 + 1, '\0');
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    out.resize(length > 0 ? size_t(length) : 0);
    return out;
}

int Count(const std::vector<std::string>& lines, const std::string& text)
{
    int count = 0;
    for (const std::string& line : lines)
        count += line.find(text) != std::string::npos ? 1 : 0;
    return count;
}

void CheckLog(const std::vector<std::string>& lines, const std::string& text, int times = 1)
{
    const int count = Count(lines, text);
    Check(count == times, "the log says \"%s\"%s (%d times)", text.c_str(), times == 1 ? "" : " each time", count);
}

// Runs this exe with --scenario <name>; its exit code is its failure count.
bool RunScenario(const std::wstring& folder, const wchar_t* name)
{
    DeleteFileW((folder + L"\\dlssnr.log").c_str());
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --scenario " + name;
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    std::fflush(stdout);
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, folder.c_str(), &startup, &process))
    {
        Check(false, "could not start the %ls scenario (%lu)", name, GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Check(exitCode == 0, "the %ls scenario passed its own checks (exit code %lu)", name, exitCode);
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring folder = Folder();
    if (argc == 3 && std::wcscmp(argv[1], L"--scenario") == 0)
    {
        const int failures = std::wcscmp(argv[2], L"early") == 0  ? Early(folder)
                             : std::wcscmp(argv[2], L"late") == 0 ? Late(folder)
                                                                  : 1;
        std::fflush(stdout);
        return failures;
    }

    std::puts("== bridge: banana.nvngx.dll in front of a fake model");
    Bridge(folder);

    std::puts("== early: dxgi.dll first, then the core (as in a game)");
    if (RunScenario(folder, L"early"))
    {
        std::puts("dlssnr.log");
        const std::vector<std::string> lines = ReadLog(folder);
        CheckLog(lines, "watching for _nvngx.dll");
        CheckLog(lines, "core loaded: ", 2);
        CheckLog(lines, "patched _nvngx.dll at ", 2);
        CheckLog(lines, "  loaded " + Utf8(folder + kSnippet));
        CheckLog(lines, "0 handles were still open", 2);
        CheckLog(lines, "CreateFeature(SR 1) -> 0x00000001, handle #100, from ngxtest.exe+", 2);
        CheckLog(lines,
                 "SR 1 #100: render 1920x1080, output 3840x2160, quality 1, flags 0x4B IsHDR MVLowRes "
                 "DepthInverted AutoExposure",
                 2);
        CheckLog(lines, "CreateFeature(FG 11) -> 0x00000001, handle #101");
        CheckLog(lines, "CreateFeature(feature 99) -> 0xBAD00005, from ngxtest.exe+");
        CheckLog(lines, "first EvaluateFeature(SR 1 #100) -> 0x00000001, from ngxtest.exe+", 2);
        CheckLog(lines, "EvaluateFeature(SR 1 #100) -> 0xBAD00005 at evaluation 4");
        CheckLog(lines, "EvaluateFeature(SR 1 #100) succeeds again after 2 failed");
        CheckLog(lines, "EvaluateFeature on a handle we did not see created");
        CheckLog(lines, "ReleaseFeature(SR 1 #100) -> 0x00000001 after 6 evaluations, 2 failed");
        CheckLog(lines, "ReleaseFeature(FG 11 #101) -> 0x00000001 after 2 evaluations, 0 failed");
        CheckLog(lines, "core Shutdown1 -> 0x00000001, from ngxtest.exe+");
        // The NR pass saw the SR evaluations, read the parameters and stood down before touching anything.
        CheckLog(lines, "settings: ");
        CheckLog(lines, "NR skipped for SR 1 #100: no Output in the parameters (logged once per reason)");
        CheckLog(lines, "process ending; totals: SR 1: 2 created, 7 evaluations, 2 failed; FG 11: 1 created, 2 "
                        "evaluations, 0 failed; unknown handles: 1 evaluations");
        CheckLog(lines, "could not", 0);
    }

    std::puts("== late: the core first, then dxgi.dll");
    if (RunScenario(folder, L"late"))
    {
        std::puts("dlssnr.log");
        const std::vector<std::string> lines = ReadLog(folder);
        CheckLog(lines, "core was already loaded when dxgi.dll started");
        CheckLog(lines, "patched _nvngx.dll at ");
        CheckLog(lines, "CreateFeature(SR 1) -> 0x00000001, handle #100");
        CheckLog(lines, "CreateFeature(RR 13)", 0);
        CheckLog(lines, "EvaluateFeature on a handle we did not see created");
        CheckLog(lines, "ReleaseFeature of a handle we did not see created");
        CheckLog(lines, "NR skipped for SR 1 #100: no parameters (logged once per reason)");
        CheckLog(lines, "process ending; totals: SR 1: 1 created, 1 evaluations, 0 failed; unknown handles: 1 "
                        "evaluations");
        CheckLog(lines, "could not", 0);
    }

    if (g_failures == 0)
        std::puts("PASS");
    else
        std::printf("FAIL (%d)\n", g_failures);
    return g_failures;
}
