// banana.nvngx.dll: the bridge through which dxgi.dll calls the Neural Rendering model, nvngx_dlssnr.dll.
//
// Why it exists: the model looks up the module that owns its caller's return address and refuses any whose path
// does not contain "nvngx.dll", before it reads a single argument; and the driver's NGX core will not load this model
// for us (it rejects the model's signature). So the model's direct caller has to be a module named like this one.
// Nothing else is decided here: the bridge knows no parameter names and no D3D12; dxgi.dll fills the parameter
// blocks, and every call below hands back exactly what the model returned.
//
// One thing is load-bearing: each result goes through a volatile local before it is returned. With
// `return g_init(...)` the compiler emits a jump instead of a call, this module's frame disappears, and the model
// sees dxgi.dll as its caller.

#include <windows.h>

#include "build_hash.h"

namespace
{
// The model's six D3D12 entry points, by signature only (Init_Ext's order, version before feature info).
using InitExt = int(__cdecl*)(unsigned long long appId, const wchar_t* dataPath, void* device, int version,
                              const void* featureInfo);
using Populate = int(__cdecl*)(void* params);
using Create = int(__cdecl*)(void* list, int feature, void* params, void** handle);
using Evaluate = int(__cdecl*)(void* list, const void* handle, void* params, void* callback);
using Release = int(__cdecl*)(void* handle);
using Shutdown1 = int(__cdecl*)(void* device);

HMODULE g_model = nullptr;
InitExt g_init = nullptr;
Populate g_populate = nullptr;
Create g_create = nullptr;
Evaluate g_evaluate = nullptr;
Release g_release = nullptr;
Shutdown1 g_shutdown = nullptr;

constexpr int kNotLoaded = int(0xBAD00007); // NVSDK_NGX_Result_FAIL_NotInitialized: bz_load did not succeed

template <typename Function> Function Find(const char* name, int bit, int* found)
{
    const auto function = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(g_model, name)));
    if (function != nullptr)
        *found |= 1 << bit;
    return function;
}
} // namespace

extern "C"
{
    // Loads the model from its full path and takes its entry points. Returns a mask of the ones found (bit 0
    // Init_Ext, 1 PopulateParameters_Impl, 2 CreateFeature, 3 EvaluateFeature, 4 ReleaseFeature, 5 Shutdown1;
    // 63 = all six), or -1 if the file would not load (GetLastError says why). Loading it again is a no-op.
    __declspec(dllexport) int bz_load(const wchar_t* path)
    {
        if (g_model == nullptr)
            g_model = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (g_model == nullptr)
            return -1;
        int found = 0;
        g_init = Find<InitExt>("NVSDK_NGX_D3D12_Init_Ext", 0, &found);
        g_populate = Find<Populate>("NVSDK_NGX_D3D12_PopulateParameters_Impl", 1, &found);
        g_create = Find<Create>("NVSDK_NGX_D3D12_CreateFeature", 2, &found);
        g_evaluate = Find<Evaluate>("NVSDK_NGX_D3D12_EvaluateFeature", 3, &found);
        g_release = Find<Release>("NVSDK_NGX_D3D12_ReleaseFeature", 4, &found);
        g_shutdown = Find<Shutdown1>("NVSDK_NGX_D3D12_Shutdown1", 5, &found);
        return found;
    }

    // The model's functions, with their own arguments and their own results (an NVSDK_NGX_Result; 1 is success).
    __declspec(dllexport) int bz_init(unsigned long long appId, const wchar_t* dataPath, void* device, int version,
                                      const void* featureInfo)
    {
        if (g_init == nullptr)
            return kNotLoaded;
        volatile int result = g_init(appId, dataPath, device, version, featureInfo);
        return result;
    }

    __declspec(dllexport) int bz_populate(void* params)
    {
        if (g_populate == nullptr)
            return kNotLoaded;
        volatile int result = g_populate(params);
        return result;
    }

    __declspec(dllexport) int bz_create(void* list, int feature, void* params, void** handle)
    {
        if (g_create == nullptr)
            return kNotLoaded;
        volatile int result = g_create(list, feature, params, handle);
        return result;
    }

    __declspec(dllexport) int bz_evaluate(void* list, const void* handle, void* params, void* callback)
    {
        if (g_evaluate == nullptr)
            return kNotLoaded;
        volatile int result = g_evaluate(list, handle, params, callback);
        return result;
    }

    __declspec(dllexport) int bz_release(void* handle)
    {
        if (g_release == nullptr)
            return kNotLoaded;
        volatile int result = g_release(handle);
        return result;
    }

    // The model's own shutdown, before the process ends: left to its DLL_PROCESS_DETACH instead, the model's
    // clean-up faults inside NvAPI.
    __declspec(dllexport) int bz_shutdown(void* device)
    {
        if (g_shutdown == nullptr)
            return kNotLoaded;
        volatile int result = g_shutdown(device);
        return result;
    }

    // The build this bridge came from. dxgi.dll only uses a bridge from its own build.
    __declspec(dllexport) const char* bz_build() { return BUILD_HASH; }
}
