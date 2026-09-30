// fakemodel: a stand-in for NVIDIA's NR model, nvngx_dlssnr.dll, for ngxtest. Built as fakemodel\nvngx_dlssnr.dll.
// It does the one thing the real model does that the bridge exists for: it looks up the module
// that owns its caller's return address and refuses the call with PlatformError unless that module's path contains
// "nvngx.dll". So a call from ngxtest.exe fails, a call through banana.nvngx.dll succeeds, and a bridge that
// tail-called into the model would fail here exactly as it would in a game. Every call is recorded so the test can
// see what arrived and from where.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <cwchar>
#include <cwctype>

#include "fakemodel.h"

namespace
{
FakeModelCall g_last = {};
unsigned g_nextId = kFakeModelFirstId;

// The real model's check, as far as it is known: RtlPcToFileHeader on the return address, then the module's path.
bool CallerAllowed(const void* caller)
{
    void* base = nullptr;
    if (RtlPcToFileHeader(const_cast<void*>(caller), &base) == nullptr)
        return false;
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(base), path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    for (DWORD i = 0; i < length; ++i)
        path[i] = wchar_t(towlower(path[i]));
    return wcsstr(path, L"nvngx.dll") != nullptr;
}

// Records the call; true if it may go on.
bool Record(const char* function, const void* a0, const void* a1, const void* a2, const void* a3, const void* caller)
{
    g_last.function = function;
    g_last.appId = 0;
    g_last.version = 0;
    g_last.args[0] = a0;
    g_last.args[1] = a1;
    g_last.args[2] = a2;
    g_last.args[3] = a3;
    g_last.caller = caller;
    g_last.accepted = CallerAllowed(caller);
    ++g_last.calls;
    return g_last.accepted;
}

const void* Widen(int value) { return reinterpret_cast<const void*>(static_cast<intptr_t>(value)); }
} // namespace

extern "C"
{
    __declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(unsigned long long appId, const wchar_t* dataPath, void* device,
                                                       int version, const void* featureInfo)
    {
        const bool allowed = Record("Init_Ext", dataPath, device, featureInfo, nullptr, _ReturnAddress());
        g_last.appId = appId;
        g_last.version = version;
        return allowed ? kFakeModelSuccess : kFakeModelPlatformError;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_PopulateParameters_Impl(void* params)
    {
        return Record("PopulateParameters_Impl", params, nullptr, nullptr, nullptr, _ReturnAddress())
                   ? kFakeModelSuccess
                   : kFakeModelPlatformError;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(void* list, int feature, void* params,
                                                            FakeModelHandle** handle)
    {
        if (!Record("CreateFeature", list, Widen(feature), params, handle, _ReturnAddress()))
            return kFakeModelPlatformError;
        if (feature != 18 || handle == nullptr)
            return kFakeModelInvalidParameter;
        *handle = new FakeModelHandle { g_nextId++ };
        return kFakeModelSuccess;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(void* list, const FakeModelHandle* handle, void* params,
                                                              void* callback)
    {
        return Record("EvaluateFeature", list, handle, params, callback, _ReturnAddress()) ? kFakeModelSuccess
                                                                                          : kFakeModelPlatformError;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(FakeModelHandle* handle)
    {
        if (!Record("ReleaseFeature", handle, nullptr, nullptr, nullptr, _ReturnAddress()))
            return kFakeModelPlatformError;
        delete handle;
        return kFakeModelSuccess;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(void* device)
    {
        return Record("Shutdown1", device, nullptr, nullptr, nullptr, _ReturnAddress()) ? kFakeModelSuccess
                                                                                    : kFakeModelPlatformError;
    }

    __declspec(dllexport) void FakeModel_LastCall(FakeModelCall* call) { *call = g_last; }
}
