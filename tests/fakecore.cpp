// fakecore: a stand-in for the driver's NGX core, for ngxtest. Built as fakecore\_nvngx.dll, the core's name, and
// copied beside it as nvngx_dlss.dll, a DLSS snippet's name, which dxgi.dll must leave alone (snippets export the same
// function names). Exports the five functions dxgi.dll intercepts, with the core's signatures, and one it must not
// touch; each call is recorded so the test can see exactly what arrived and from where.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include "fakecore.h"

namespace
{
FakeCall g_last = {};
int g_evaluateResult = kFakeSuccess;
unsigned g_nextId = kFakeFirstId;

void Record(const char* function, const void* a0, const void* a1, const void* a2, const void* a3, const void* caller)
{
    g_last.function = function;
    g_last.args[0] = a0;
    g_last.args[1] = a1;
    g_last.args[2] = a2;
    g_last.args[3] = a3;
    g_last.caller = caller;
    ++g_last.calls;
}
} // namespace

extern "C"
{
    __declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(void* list, int feature, void* params, FakeHandle** handle)
    {
        Record("CreateFeature", list, reinterpret_cast<void*>(static_cast<intptr_t>(feature)), params, handle,
               _ReturnAddress());
        if (feature == kFakeRefusedFeature)
            return kFakeInvalidParameter;
        *handle = new FakeHandle { g_nextId++ };
        return kFakeSuccess;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(void* list, const FakeHandle* handle, void* params,
                                                              void* callback)
    {
        Record("EvaluateFeature", list, handle, params, callback, _ReturnAddress());
        return g_evaluateResult;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(FakeHandle* handle)
    {
        Record("ReleaseFeature", handle, nullptr, nullptr, nullptr, _ReturnAddress());
        delete handle;
        return kFakeSuccess;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(void* device)
    {
        Record("Shutdown1", device, nullptr, nullptr, nullptr, _ReturnAddress());
        return kFakeSuccess;
    }

    __declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown()
    {
        Record("Shutdown", nullptr, nullptr, nullptr, nullptr, _ReturnAddress());
        return kFakeSuccess;
    }

    // Not intercepted: its export table entry must stay as built.
    __declspec(dllexport) int NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, void*, void*, int)
    {
        Record("Init", nullptr, nullptr, nullptr, nullptr, _ReturnAddress());
        return kFakeSuccess;
    }

    __declspec(dllexport) void FakeCore_LastCall(FakeCall* call) { *call = g_last; }

    __declspec(dllexport) void FakeCore_SetEvaluateResult(int result) { g_evaluateResult = result; }
}
