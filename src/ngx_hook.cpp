// Interception of the game's DLSS calls. The interface is in ngx_hook.h.
//
// When the driver's NGX core (_nvngx.dll) loads, the loader tells us before LoadLibrary returns to whoever loaded it.
// We then point five entries of the core's export table at small trampolines of ours, so every GetProcAddress for
// those names from then on -- the game's NGX library, Streamline's -- gets our function, which calls the core's own.
// No code byte of the core changes. CreateFeature, EvaluateFeature and ReleaseFeature count and log; since M2, a
// successful SR/RR evaluation is also followed by the Neural Rendering pass on the same command list (nr_dx12.cpp),
// and Shutdown and Shutdown1 let that pass shut the model down before the game shuts the core down.

#include "ngx_hook.h"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "log.h"
#include "menu.h"
#include "nr_dx12.h"
#include "nvsdk_ngx.h"

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// The loader's DLL notification. Documented, but declared in no SDK header.

struct LdrString
{
    USHORT Length; // bytes
    USHORT MaximumLength;
    PCWSTR Buffer;
};

struct LdrNotification
{
    ULONG Flags;
    const LdrString* FullDllName;
    const LdrString* BaseDllName;
    void* DllBase;
    ULONG SizeOfImage;
};

constexpr ULONG kDllLoaded = 1;
constexpr ULONG kDllUnloaded = 2;

using LdrCallback = VOID(CALLBACK*)(ULONG reason, const LdrNotification* data, PVOID context);
using LdrRegister = LONG(NTAPI*)(ULONG flags, LdrCallback callback, PVOID context, PVOID* cookie);

// ---------------------------------------------------------------------------------------------------------------
// Names and text.

constexpr wchar_t kCoreName[] = L"_nvngx.dll";

wchar_t Lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; }

// Whether the first `length` characters of `text` are `prefix` (all of it), ignoring ASCII case.
bool StartsWith(const wchar_t* text, size_t length, const wchar_t* prefix)
{
    size_t i = 0;
    for (; prefix[i] != L'\0'; ++i)
    {
        if (i == length || Lower(text[i]) != Lower(prefix[i]))
            return false;
    }
    return true;
}

bool Equals(const wchar_t* text, size_t length, const wchar_t* name)
{
    return wcslen(name) == length && StartsWith(text, length, name);
}

// UTF-8 for the log; paths can hold any character.
void Utf8(const wchar_t* text, int length, char* out, int size)
{
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, length, out, size - 1, nullptr, nullptr);
    out[written > 0 ? written : 0] = '\0';
}

// "sl.dlss.dll+0x1A2B3", or the bare address if no module holds it.
void DescribeAddress(const void* address, char* out, size_t size)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    DWORD length = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module))
        length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        snprintf(out, size, "%p", address);
        return;
    }
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash != nullptr ? slash + 1 : path;
    char name8[MAX_PATH * 3];
    Utf8(name, -1, name8, int(std::size(name8)));
    snprintf(
        out, size, "%s+0x%llX", name8,
        static_cast<unsigned long long>(static_cast<const BYTE*>(address) - reinterpret_cast<const BYTE*>(module)));
}

// Feature numbers: SR 1, FG 11, RR 13, NR 18 (18 has no name in the SDK headers).
constexpr unsigned kFeatureSR = NVSDK_NGX_Feature_SuperSampling;
constexpr unsigned kFeatureFG = NVSDK_NGX_Feature_FrameGeneration;
constexpr unsigned kFeatureRR = NVSDK_NGX_Feature_RayReconstruction;
constexpr unsigned kFeatureNR = 18;

// "SR 1", "FG 11", "feature 5".
void FeatureLabel(unsigned feature, char* out, size_t size)
{
    const char* name = feature == kFeatureSR   ? "SR"
                       : feature == kFeatureFG ? "FG"
                       : feature == kFeatureRR ? "RR"
                       : feature == kFeatureNR ? "NR"
                                               : nullptr;
    if (name != nullptr)
        snprintf(out, size, "%s %u", name, feature);
    else
        snprintf(out, size, "feature %u", feature);
}

// Appends to out[] at *length, never past size.
void Append(char* out, size_t size, size_t* length, const char* format, ...)
{
    if (*length + 1 >= size)
        return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *length, size - *length, format, args);
    va_end(args);
    if (written > 0)
        *length = *length + written < size ? *length + written : size - 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Counts. Per feature number, and per created feature (a handle). Evaluate only adds to atomics; Create and Release,
// which happen a few times per session, take a lock.

constexpr unsigned kFeatureSlots = 32; // feature numbers 0..30; anything higher is counted in the last slot

unsigned Slot(unsigned feature) { return feature < kFeatureSlots - 1 ? feature : kFeatureSlots - 1; }

struct FeatureCounts
{
    std::atomic<uint64_t> created { 0 };
    std::atomic<uint64_t> evaluates { 0 };
    std::atomic<uint64_t> failed { 0 };
};
FeatureCounts g_counts[kFeatureSlots];

// Evaluations of a handle we did not see created: made before we patched, or through an address taken before.
std::atomic<uint64_t> g_unknownEvaluates { 0 };

struct Tracked
{
    std::atomic<const NVSDK_NGX_Handle*> handle { nullptr }; // nullptr: slot free
    std::atomic<unsigned> feature { 0 };
    std::atomic<unsigned> id { 0 }; // NVSDK_NGX_Handle::Id, for the log
    std::atomic<uint64_t> evaluates { 0 };
    std::atomic<uint64_t> failed { 0 };
    std::atomic<uint64_t> failRun { 0 }; // failures in a row, up to now
    std::atomic<bool> evaluated { false };
    // The creation parameters the NR pass needs (nr_dx12.h); 0 where the game did not give them.
    std::atomic<unsigned> outWidth { 0 };
    std::atomic<unsigned> outHeight { 0 };
    std::atomic<unsigned> renderWidth { 0 };
    std::atomic<unsigned> renderHeight { 0 };
    std::atomic<int> createFlags { 0 };
};

// What CreateFeature(SR/RR) was given, as far as we read it.
struct CreateInfo
{
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;
    int quality = -1, flags = 0;
    bool haveSize = false, haveOut = false, haveQuality = false, haveFlags = false;
};

constexpr size_t kMaxTracked = 64;
Tracked g_tracked[kMaxTracked];
std::atomic<size_t> g_trackedEnd { 0 }; // slots [0, end) have been used
SRWLOCK g_trackLock = SRWLOCK_INIT;

// Lock-free: the evaluate path.
Tracked* Find(const NVSDK_NGX_Handle* handle)
{
    const size_t end = g_trackedEnd.load(std::memory_order_acquire);
    for (size_t i = 0; i < end; ++i)
    {
        if (g_tracked[i].handle.load(std::memory_order_acquire) == handle)
            return &g_tracked[i];
    }
    return nullptr;
}

// False if the table is full.
bool Track(const NVSDK_NGX_Handle* handle, unsigned feature, unsigned id, const CreateInfo& info)
{
    AcquireSRWLockExclusive(&g_trackLock);
    const size_t end = g_trackedEnd.load(std::memory_order_relaxed);
    Tracked* slot = nullptr;
    for (size_t i = 0; i < end && slot == nullptr; ++i)
    {
        // A handle the core hands out again without us seeing its release takes over its old slot.
        const NVSDK_NGX_Handle* held = g_tracked[i].handle.load(std::memory_order_relaxed);
        if (held == nullptr || held == handle)
            slot = &g_tracked[i];
    }
    if (slot == nullptr && end < kMaxTracked)
        slot = &g_tracked[end];
    if (slot != nullptr)
    {
        slot->handle.store(nullptr, std::memory_order_relaxed);
        slot->feature.store(feature, std::memory_order_relaxed);
        slot->id.store(id, std::memory_order_relaxed);
        slot->evaluates.store(0, std::memory_order_relaxed);
        slot->failed.store(0, std::memory_order_relaxed);
        slot->failRun.store(0, std::memory_order_relaxed);
        slot->evaluated.store(false, std::memory_order_relaxed);
        slot->outWidth.store(info.outWidth, std::memory_order_relaxed);
        slot->outHeight.store(info.outHeight, std::memory_order_relaxed);
        slot->renderWidth.store(info.width, std::memory_order_relaxed);
        slot->renderHeight.store(info.height, std::memory_order_relaxed);
        slot->createFlags.store(info.flags, std::memory_order_relaxed);
        slot->handle.store(handle, std::memory_order_release);
        if (slot == &g_tracked[end])
            g_trackedEnd.store(end + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_trackLock);
    return slot != nullptr;
}

struct TrackedCopy
{
    unsigned feature;
    unsigned id;
    uint64_t evaluates;
    uint64_t failed;
};

// Takes the handle out of the table. False if it was not in it.
bool Untrack(const NVSDK_NGX_Handle* handle, TrackedCopy* copy)
{
    AcquireSRWLockExclusive(&g_trackLock);
    Tracked* slot = Find(handle);
    if (slot != nullptr)
    {
        *copy = { slot->feature.load(), slot->id.load(), slot->evaluates.load(), slot->failed.load() };
        slot->handle.store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_trackLock);
    return slot != nullptr;
}

// When the core unloads, every handle it gave out is gone with it. Returns how many were still open.
size_t UntrackAll()
{
    AcquireSRWLockExclusive(&g_trackLock);
    size_t open = 0;
    const size_t end = g_trackedEnd.load(std::memory_order_relaxed);
    for (size_t i = 0; i < end; ++i)
    {
        if (g_tracked[i].handle.exchange(nullptr, std::memory_order_acq_rel) != nullptr)
            ++open;
    }
    ReleaseSRWLockExclusive(&g_trackLock);
    return open;
}

// ---------------------------------------------------------------------------------------------------------------
// The reporter: one line with how many evaluations of each feature there were since its last line, and how many
// failed, then the totals since start (a game that exits without DLL_PROCESS_DETACH, as TLOU2 does, never writes
// the totals line, so every report line carries them). It is its own thread so that the evaluate path never writes
// to the log; it only starts once the game creates a feature, and writes nothing while nothing is evaluated. Every
// 5 s at first, so a short scripted run shows the rate; from the fifth line on the interval doubles, up to 5
// minutes, so a long session adds a dozen lines an hour, not a line every few seconds.

// "SR 1: 2 created, 7 evaluations, 2 failed; FG 11: ...; unknown handles: 1 evaluations; NR 1200 frames, ...".
// Returns 0 if there is nothing to say.
size_t DescribeTotals(char* out, size_t size)
{
    size_t length = 0;
    for (unsigned slot = 0; slot < kFeatureSlots; ++slot)
    {
        const uint64_t created = g_counts[slot].created.load();
        const uint64_t evaluates = g_counts[slot].evaluates.load();
        if (created == 0 && evaluates == 0)
            continue;
        char label[32];
        FeatureLabel(slot, label, sizeof label);
        Append(out, size, &length, "%s%s: %llu created, %llu evaluations, %llu failed", length == 0 ? "" : "; ", label,
               static_cast<unsigned long long>(created), static_cast<unsigned long long>(evaluates),
               static_cast<unsigned long long>(g_counts[slot].failed.load()));
    }
    const uint64_t unknown = g_unknownEvaluates.load();
    if (unknown != 0)
        Append(out, size, &length, "%sunknown handles: %llu evaluations", length == 0 ? "" : "; ",
               static_cast<unsigned long long>(unknown));
    char nr[256];
    if (NrDescribe(nr, sizeof nr) != 0)
        Append(out, size, &length, "%s%s", length == 0 ? "" : "; ", nr);
    return length;
}

constexpr DWORD kReportMilliseconds = 5000;
constexpr DWORD kReportMaxMilliseconds = 300000;
constexpr unsigned kReportsBeforeBackoff = 4;

DWORD WINAPI Report(void*)
{
    uint64_t lastEvaluates[kFeatureSlots] = {};
    uint64_t lastFailed[kFeatureSlots] = {};
    uint64_t lastUnknown = 0;
    bool quiet = true;
    double then = LogClock();
    DWORD interval = kReportMilliseconds;
    for (unsigned reports = 0;; ++reports)
    {
        if (reports >= kReportsBeforeBackoff && interval < kReportMaxMilliseconds)
            interval = interval * 2 < kReportMaxMilliseconds ? interval * 2 : kReportMaxMilliseconds;
        Sleep(interval);
        const double now = LogClock();
        const double seconds = now - then;
        then = now;

        char line[768];
        size_t length = 0;
        for (unsigned slot = 0; slot < kFeatureSlots; ++slot)
        {
            const uint64_t evaluates = g_counts[slot].evaluates.load(std::memory_order_relaxed);
            const uint64_t failed = g_counts[slot].failed.load(std::memory_order_relaxed);
            const uint64_t newEvaluates = evaluates - lastEvaluates[slot];
            const uint64_t newFailed = failed - lastFailed[slot];
            lastEvaluates[slot] = evaluates;
            lastFailed[slot] = failed;
            if (newEvaluates == 0)
                continue;
            char label[32];
            FeatureLabel(slot, label, sizeof label);
            Append(line, sizeof line, &length, "%s%s %llu (%.1f/s)", length == 0 ? "" : ", ", label,
                   static_cast<unsigned long long>(newEvaluates), double(newEvaluates) / seconds);
            if (newFailed != 0)
                Append(line, sizeof line, &length, " of which %llu failed", static_cast<unsigned long long>(newFailed));
        }
        const uint64_t unknown = g_unknownEvaluates.load(std::memory_order_relaxed);
        if (unknown != lastUnknown)
            Append(line, sizeof line, &length, "%sunknown handles %llu", length == 0 ? "" : ", ",
                   static_cast<unsigned long long>(unknown - lastUnknown));
        lastUnknown = unknown;

        if (length != 0)
        {
            char totals[768];
            DescribeTotals(totals, sizeof totals);
            Log("evaluations in the last %.1f s: %s; totals: %s", seconds, line, totals);
            quiet = false;
        }
        else if (!quiet)
        {
            Log("no evaluations in the last %.1f s", seconds);
            quiet = true;
        }
    }
}

void StartReporter()
{
    static std::atomic<bool> started { false };
    if (started.exchange(true))
        return;
    const HANDLE thread = CreateThread(nullptr, 0, &Report, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
        Log("could not start the reporter thread (%lu); only events are logged", GetLastError());
}

// ---------------------------------------------------------------------------------------------------------------
// What happens around each intercepted call. The rare branches (first call, failures) are kept out of line so the
// evaluate path stays a table lookup and a few atomic adds.

std::atomic<void*> g_realCreate { nullptr };
std::atomic<void*> g_realEvaluate { nullptr };
std::atomic<void*> g_realRelease { nullptr };
std::atomic<void*> g_realShutdown1 { nullptr };
std::atomic<void*> g_realShutdown { nullptr };

using CreateFeature = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFeature = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFeature = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
using CoreShutdown1 = decltype(&NVSDK_NGX_D3D12_Shutdown1);
using CoreShutdown = NVSDK_NGX_Result(NVSDK_CONV*)(); // the SDK declares it only under NGX_ENABLE_DEPRECATED_SHUTDOWN

// The SR/RR creation parameters the NR pass depends on. Only read; the game's parameters stay as they are.
CreateInfo ReadCreateParameters(const NVSDK_NGX_Parameter* params)
{
    CreateInfo info;
    info.haveSize = params->Get(NVSDK_NGX_Parameter_Width, &info.width) == NVSDK_NGX_Result_Success &&
                    params->Get(NVSDK_NGX_Parameter_Height, &info.height) == NVSDK_NGX_Result_Success;
    info.haveOut = params->Get(NVSDK_NGX_Parameter_OutWidth, &info.outWidth) == NVSDK_NGX_Result_Success &&
                   params->Get(NVSDK_NGX_Parameter_OutHeight, &info.outHeight) == NVSDK_NGX_Result_Success;
    info.haveQuality = params->Get(NVSDK_NGX_Parameter_PerfQualityValue, &info.quality) == NVSDK_NGX_Result_Success;
    info.haveFlags =
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &info.flags) == NVSDK_NGX_Result_Success;
    if (!info.haveSize)
        info.width = info.height = 0;
    if (!info.haveOut)
        info.outWidth = info.outHeight = 0;
    if (!info.haveFlags)
        info.flags = 0;
    return info;
}

void LogCreateParameters(const char* label, unsigned id, const CreateInfo& info)
{
    const unsigned width = info.width, height = info.height, outWidth = info.outWidth, outHeight = info.outHeight;
    const int quality = info.quality, flags = info.flags;
    char text[256];
    size_t length = 0;
    if (info.haveSize)
        Append(text, sizeof text, &length, "render %ux%u", width, height);
    else
        Append(text, sizeof text, &length, "render ?");
    if (info.haveOut)
        Append(text, sizeof text, &length, ", output %ux%u", outWidth, outHeight);
    else
        Append(text, sizeof text, &length, ", output ?");
    if (info.haveQuality)
        Append(text, sizeof text, &length, ", quality %d", quality);
    if (info.haveFlags)
    {
        Append(text, sizeof text, &length, ", flags 0x%X", unsigned(flags));
        const struct
        {
            int bit;
            const char* name;
        } names[] = {
            { NVSDK_NGX_DLSS_Feature_Flags_IsHDR, "IsHDR" },
            { NVSDK_NGX_DLSS_Feature_Flags_MVLowRes, "MVLowRes" },
            { NVSDK_NGX_DLSS_Feature_Flags_MVJittered, "MVJittered" },
            { NVSDK_NGX_DLSS_Feature_Flags_DepthInverted, "DepthInverted" },
            { NVSDK_NGX_DLSS_Feature_Flags_DoSharpening, "DoSharpening" },
            { NVSDK_NGX_DLSS_Feature_Flags_AutoExposure, "AutoExposure" },
            { NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling, "AlphaUpscaling" },
        };
        for (const auto& name : names)
        {
            if (flags & name.bit)
                Append(text, sizeof text, &length, " %s", name.name);
        }
    }
    Log("  %s #%u: %s", label, id, text);
}

void OnCreate(unsigned feature, const NVSDK_NGX_Parameter* params, const NVSDK_NGX_Handle* handle,
              NVSDK_NGX_Result result, const void* caller)
{
    StartReporter();
    char label[32];
    FeatureLabel(feature, label, sizeof label);
    char from[MAX_PATH * 3 + 32];
    DescribeAddress(caller, from, sizeof from);
    if (handle == nullptr)
    {
        Log("CreateFeature(%s) -> 0x%08X, from %s, thread %lu", label, unsigned(result), from, GetCurrentThreadId());
        return;
    }

    const unsigned id = handle->Id;
    g_counts[Slot(feature)].created.fetch_add(1, std::memory_order_relaxed);
    const bool upscaler = (feature == kFeatureSR || feature == kFeatureRR) && params != nullptr;
    const CreateInfo info = upscaler ? ReadCreateParameters(params) : CreateInfo();
    const bool tracked = Track(handle, feature, id, info);
    Log("CreateFeature(%s) -> 0x%08X, handle #%u, from %s, thread %lu%s", label, unsigned(result), id, from,
        GetCurrentThreadId(), tracked ? "" : "; the handle table is full, its evaluations count as unknown");
    if (upscaler)
        LogCreateParameters(label, id, info);
}

__declspec(noinline) void LogFirstEvaluate(const Tracked& tracked, NVSDK_NGX_Result result, const void* caller)
{
    char label[32];
    FeatureLabel(tracked.feature.load(std::memory_order_relaxed), label, sizeof label);
    char from[MAX_PATH * 3 + 32];
    DescribeAddress(caller, from, sizeof from);
    Log("first EvaluateFeature(%s #%u) -> 0x%08X, from %s, thread %lu", label,
        tracked.id.load(std::memory_order_relaxed), unsigned(result), from, GetCurrentThreadId());
}

__declspec(noinline) void LogFailing(const Tracked& tracked, NVSDK_NGX_Result result)
{
    char label[32];
    FeatureLabel(tracked.feature.load(std::memory_order_relaxed), label, sizeof label);
    Log("EvaluateFeature(%s #%u) -> 0x%08X at evaluation %llu", label, tracked.id.load(std::memory_order_relaxed),
        unsigned(result), static_cast<unsigned long long>(tracked.evaluates.load(std::memory_order_relaxed)));
}

__declspec(noinline) void LogRecovered(const Tracked& tracked, uint64_t failures)
{
    char label[32];
    FeatureLabel(tracked.feature.load(std::memory_order_relaxed), label, sizeof label);
    Log("EvaluateFeature(%s #%u) succeeds again after %llu failed", label, tracked.id.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(failures));
}

__declspec(noinline) void LogUnknown(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result, const void* caller)
{
    char from[MAX_PATH * 3 + 32];
    DescribeAddress(caller, from, sizeof from);
    Log("EvaluateFeature on a handle we did not see created (%p) -> 0x%08X, from %s, thread %lu; counted as unknown "
        "from now on",
        static_cast<const void*>(handle), unsigned(result), from, GetCurrentThreadId());
}

// Returns the handle's slot, or nullptr for one we did not see created.
const Tracked* OnEvaluate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result, const void* caller)
{
    Tracked* tracked = Find(handle);
    if (tracked == nullptr)
    {
        if (g_unknownEvaluates.fetch_add(1, std::memory_order_relaxed) == 0)
            LogUnknown(handle, result, caller);
        return nullptr;
    }

    FeatureCounts& counts = g_counts[Slot(tracked->feature.load(std::memory_order_relaxed))];
    counts.evaluates.fetch_add(1, std::memory_order_relaxed);
    tracked->evaluates.fetch_add(1, std::memory_order_relaxed);
    if (!tracked->evaluated.load(std::memory_order_relaxed) && !tracked->evaluated.exchange(true))
        LogFirstEvaluate(*tracked, result, caller);

    if (result == NVSDK_NGX_Result_Success)
    {
        if (tracked->failRun.load(std::memory_order_relaxed) != 0)
            LogRecovered(*tracked, tracked->failRun.exchange(0));
    }
    else
    {
        counts.failed.fetch_add(1, std::memory_order_relaxed);
        tracked->failed.fetch_add(1, std::memory_order_relaxed);
        if (tracked->failRun.fetch_add(1) == 0)
            LogFailing(*tracked, result);
    }
    return tracked;
}

void OnRelease(const NVSDK_NGX_Handle* handle, bool known, const TrackedCopy& copy, NVSDK_NGX_Result result,
               const void* caller)
{
    if (!known)
    {
        char from[MAX_PATH * 3 + 32];
        DescribeAddress(caller, from, sizeof from);
        Log("ReleaseFeature of a handle we did not see created (%p) -> 0x%08X, from %s",
            static_cast<const void*>(handle), unsigned(result), from);
        return;
    }
    char label[32];
    FeatureLabel(copy.feature, label, sizeof label);
    Log("ReleaseFeature(%s #%u) -> 0x%08X after %llu evaluations, %llu failed", label, copy.id, unsigned(result),
        static_cast<unsigned long long>(copy.evaluates), static_cast<unsigned long long>(copy.failed));
    if (copy.feature == kFeatureSR || copy.feature == kFeatureRR)
        NrSourceReleased(copy.id);
}

// ---------------------------------------------------------------------------------------------------------------
// Our five functions. The core's export table leads here (through a trampoline); each calls the core's own function
// with exactly what it was given and returns exactly what that returned.

NVSDK_NGX_Result NVSDK_CONV HookCreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                              NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle)
{
    const auto real = reinterpret_cast<CreateFeature>(g_realCreate.load(std::memory_order_acquire));
    if (real == nullptr) // the core has been unloaded and this address was kept: the call could only have crashed
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    const NVSDK_NGX_Result result = real(list, feature, params, handle);
    OnCreate(unsigned(feature), params, NVSDK_NGX_SUCCEED(result) && handle != nullptr ? *handle : nullptr, result,
             _ReturnAddress());
    return result;
}

NVSDK_NGX_Result NVSDK_CONV HookEvaluateFeature(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                                NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
{
    const auto real = reinterpret_cast<EvaluateFeature>(g_realEvaluate.load(std::memory_order_acquire));
    if (real == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    const NVSDK_NGX_Result result = real(list, handle, params, callback);
    const Tracked* tracked = OnEvaluate(handle, result, _ReturnAddress());
    // The NR pass: only after the core succeeded, only for the upscalers, on the same command list.
    // Whatever it does, the game gets the core's own result.
    if (tracked != nullptr && result == NVSDK_NGX_Result_Success)
    {
        const unsigned feature = tracked->feature.load(std::memory_order_relaxed);
        if (feature == kFeatureSR || feature == kFeatureRR)
        {
            const NrSource source = { feature,
                                      tracked->id.load(std::memory_order_relaxed),
                                      tracked->outWidth.load(std::memory_order_relaxed),
                                      tracked->outHeight.load(std::memory_order_relaxed),
                                      tracked->renderWidth.load(std::memory_order_relaxed),
                                      tracked->renderHeight.load(std::memory_order_relaxed),
                                      tracked->createFlags.load(std::memory_order_relaxed) };
            MenuOnEvaluate(list);
            NrAfterEvaluate(list, params, source);
        }
    }
    return result;
}

NVSDK_NGX_Result NVSDK_CONV HookReleaseFeature(NVSDK_NGX_Handle* handle)
{
    const auto real = reinterpret_cast<ReleaseFeature>(g_realRelease.load(std::memory_order_acquire));
    if (real == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    // Out of the table first: once the core has released it, it may hand the same handle to the next Create.
    TrackedCopy copy = {};
    const bool known = Untrack(handle, &copy);
    const NVSDK_NGX_Result result = real(handle);
    OnRelease(handle, known, copy, result, _ReturnAddress());
    return result;
}

// The game is shutting the core down (Streamline does at exit). The NR pass goes first, while the core, NvAPI and
// the device are all still there: it releases the model's features and calls the model's own Shutdown1. Left to
// its DLL_PROCESS_DETACH instead, the model's clean-up faults inside NvAPI.
void OnShutdown(const char* name, NVSDK_NGX_Result result, const void* caller)
{
    char from[MAX_PATH * 3 + 32];
    DescribeAddress(caller, from, sizeof from);
    Log("core %s -> 0x%08X, from %s", name, unsigned(result), from);
}

NVSDK_NGX_Result NVSDK_CONV HookShutdown1(ID3D12Device* device)
{
    const auto real = reinterpret_cast<CoreShutdown1>(g_realShutdown1.load(std::memory_order_acquire));
    if (real == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    MenuBeforeCoreShutdown();
    NrBeforeCoreShutdown();
    const NVSDK_NGX_Result result = real(device);
    OnShutdown("Shutdown1", result, _ReturnAddress());
    return result;
}

NVSDK_NGX_Result NVSDK_CONV HookShutdown()
{
    const auto real = reinterpret_cast<CoreShutdown>(g_realShutdown.load(std::memory_order_acquire));
    if (real == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    MenuBeforeCoreShutdown();
    NrBeforeCoreShutdown();
    const NVSDK_NGX_Result result = real();
    OnShutdown("Shutdown", result, _ReturnAddress());
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// The patch.

struct Intercepted
{
    const char* name;
    void* hook;
    std::atomic<void*>* real;
};

const Intercepted kIntercepted[] = {
    { "NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&HookCreateFeature), &g_realCreate },
    { "NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluateFeature), &g_realEvaluate },
    { "NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void*>(&HookReleaseFeature), &g_realRelease },
    { "NVSDK_NGX_D3D12_Shutdown1", reinterpret_cast<void*>(&HookShutdown1), &g_realShutdown1 },
    { "NVSDK_NGX_D3D12_Shutdown", reinterpret_cast<void*>(&HookShutdown), &g_realShutdown },
};
constexpr size_t kInterceptedCount = std::size(kIntercepted);

// An export table entry is a 32-bit offset from the module's base, so what it points at must lie above the base and
// within reach. 2 GB rather than 4: an offset is then also valid for anyone who reads it as signed.
constexpr uintptr_t kReach = 0x7FFF0000;

// Trampoline: jmp qword ptr [rip+0], then the 8-byte address. Changes no register, so the hook sees the call exactly
// as the caller made it.
constexpr size_t kTrampolineSize = 16;

SRWLOCK g_patchLock = SRWLOCK_INIT;
const BYTE* g_patchedCore = nullptr; // the core image whose table points at us; under g_patchLock
std::atomic<const BYTE*> g_coreModule { nullptr }; // the same, for NgxCoreModule()

// The export table entry for `name`, or nullptr.
DWORD* FindExport(BYTE* base, const IMAGE_EXPORT_DIRECTORY* exports, const char* name)
{
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* indices = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
    {
        if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
            return indices[i] < exports->NumberOfFunctions ? &functions[indices[i]] : nullptr;
    }
    return nullptr;
}

// One committed page, executable once written, somewhere in [base + size, base + kReach).
BYTE* AllocateNear(BYTE* base, SIZE_T size)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t granularity = system.dwAllocationGranularity;
    const uintptr_t limit = uintptr_t(base) + kReach;
    uintptr_t address = (uintptr_t(base) + size + granularity - 1) & ~(granularity - 1);
    while (address + system.dwPageSize <= limit)
    {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof region) == 0)
            return nullptr;
        if (region.State == MEM_FREE)
        {
            void* page = VirtualAlloc(reinterpret_cast<void*>(address), system.dwPageSize, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
            if (page != nullptr)
                return static_cast<BYTE*>(page);
        }
        const uintptr_t next = uintptr_t(region.BaseAddress) + region.RegionSize;
        address = (next + granularity - 1) & ~(granularity - 1);
    }
    return nullptr;
}

bool WriteEntry(DWORD* entry, DWORD value)
{
    DWORD protection;
    if (!VirtualProtect(entry, sizeof *entry, PAGE_READWRITE, &protection))
        return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), LONG(value));
    DWORD unused;
    VirtualProtect(entry, sizeof *entry, protection, &unused);
    return true;
}

// Points the five entries of the core at us. All or nothing: if any step fails the core is left as it was and the
// game runs without interception. Runs under the loader lock (from the notification or DllMain): only memory calls
// and the log.
void PatchCore(BYTE* base, const char* path)
{
    AcquireSRWLockExclusive(&g_patchLock);
    if (g_patchedCore == base)
    {
        ReleaseSRWLockExclusive(&g_patchLock);
        return;
    }
    if (g_patchedCore != nullptr)
    {
        // Our functions call one core; a second one loaded beside it (from another folder) is left alone.
        const void* patched = g_patchedCore;
        ReleaseSRWLockExclusive(&g_patchLock);
        Log("not patching %s: another _nvngx.dll is already patched at %p", path, patched);
        return;
    }

    const char* problem = nullptr;
    DWORD* entries[kInterceptedCount] = {};
    DWORD original[kInterceptedCount] = {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY* directory = nullptr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        problem = "not a 64-bit PE image";
    else
    {
        directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory->VirtualAddress == 0 || directory->Size == 0)
            problem = "it has no export table";
    }
    const SIZE_T imageSize = problem == nullptr ? nt->OptionalHeader.SizeOfImage : 0;
    for (size_t i = 0; i < kInterceptedCount && problem == nullptr; ++i)
    {
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory->VirtualAddress);
        entries[i] = FindExport(base, exports, kIntercepted[i].name);
        original[i] = entries[i] != nullptr ? *entries[i] : 0;
        if (entries[i] == nullptr || original[i] == 0)
            problem = "an export is missing";
        else if (original[i] >= directory->VirtualAddress && original[i] < directory->VirtualAddress + directory->Size)
            problem = "an export is forwarded to another DLL";
    }

    BYTE* trampolines = problem == nullptr ? AllocateNear(base, imageSize) : nullptr;
    if (problem == nullptr && trampolines == nullptr)
        problem = "no free memory within 2 GB above it";
    if (problem == nullptr)
    {
        for (size_t i = 0; i < kInterceptedCount; ++i)
        {
            BYTE* trampoline = trampolines + i * kTrampolineSize;
            const BYTE jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            memcpy(trampoline, jump, sizeof jump);
            memcpy(trampoline + sizeof jump, &kIntercepted[i].hook, sizeof(void*));
            memset(trampoline + sizeof jump + sizeof(void*), 0xCC, kTrampolineSize - sizeof jump - sizeof(void*));
        }
        SYSTEM_INFO system;
        GetSystemInfo(&system);
        DWORD unused;
        if (!VirtualProtect(trampolines, system.dwPageSize, PAGE_EXECUTE_READ, &unused))
            problem = "its trampolines could not be made executable";
        else
            FlushInstructionCache(GetCurrentProcess(), trampolines, system.dwPageSize);
    }

    // The core's own functions, before anything can reach ours.
    if (problem == nullptr)
    {
        for (size_t i = 0; i < kInterceptedCount; ++i)
            kIntercepted[i].real->store(base + original[i], std::memory_order_release);
    }

    size_t written = 0;
    while (problem == nullptr && written < kInterceptedCount)
    {
        if (WriteEntry(entries[written], DWORD(trampolines + written * kTrampolineSize - base)))
            ++written;
        else
            problem = "its export table could not be written";
    }
    if (problem != nullptr)
    {
        // Put back what was already written. The trampolines stay: someone may already hold one.
        for (size_t i = 0; i < written; ++i)
            WriteEntry(entries[i], original[i]);
        ReleaseSRWLockExclusive(&g_patchLock);
        Log("could not patch %s: %s; the game runs without interception", path, problem);
        return;
    }
    g_patchedCore = base;
    g_coreModule.store(base, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_patchLock);

    for (size_t i = 0; i < kInterceptedCount; ++i)
    {
        // Someone else (an overlay, another mod) may have pointed the entry elsewhere before us; we call on to that.
        const bool outside = original[i] >= imageSize;
        Log("  %s: core+0x%lX -> trampoline %p -> dxgi.dll%s", kIntercepted[i].name, original[i],
            static_cast<void*>(trampolines + i * kTrampolineSize),
            outside ? " (the entry already led outside the core; we call on to where it led)" : "");
    }
    Log("patched _nvngx.dll at %p: its CreateFeature, EvaluateFeature, ReleaseFeature, Shutdown1 and Shutdown now lead "
        "here",
        base);
}

// The core is being unloaded: nothing of it may be called any more.
void ForgetCore(const BYTE* base, const char* path)
{
    AcquireSRWLockExclusive(&g_patchLock);
    const bool patched = g_patchedCore == base;
    if (patched)
    {
        g_patchedCore = nullptr;
        g_coreModule.store(nullptr, std::memory_order_release);
        for (const Intercepted& intercepted : kIntercepted)
            intercepted.real->store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    const size_t open = patched ? UntrackAll() : 0;
    if (patched)
        NrCoreGone();
    Log("core unloaded: %s%s, %zu handles were still open", path, patched ? "" : " (it was not patched)", open);
}

VOID CALLBACK OnDllNotification(ULONG reason, const LdrNotification* data, PVOID)
{
    // Under the loader lock: memory and the log only.
    const LdrString& name = *data->BaseDllName;
    const size_t nameLength = name.Length / sizeof(wchar_t);
    const bool core = Equals(name.Buffer, nameLength, kCoreName);
    if (!core && !StartsWith(name.Buffer, nameLength, L"nvngx") && !StartsWith(name.Buffer, nameLength, L"sl."))
        return;

    char path[MAX_PATH * 3];
    Utf8(data->FullDllName->Buffer, data->FullDllName->Length / sizeof(wchar_t), path, int(std::size(path)));
    if (!core)
    {
        // The DLSS snippets, Streamline's modules: when they come and go shows the game's DLSS set-up.
        Log("%s %s", reason == kDllLoaded ? "loaded" : "unloaded", path);
        return;
    }
    if (reason == kDllLoaded)
    {
        Log("core loaded: %s at %p, %lu bytes, thread %lu", path, data->DllBase, data->SizeOfImage,
            GetCurrentThreadId());
        PatchCore(static_cast<BYTE*>(data->DllBase), path);
    }
    else if (reason == kDllUnloaded)
        ForgetCore(static_cast<const BYTE*>(data->DllBase), path);
}

bool g_started = false;
} // namespace

bool NgxHookStart()
{
    const auto registerNotification = reinterpret_cast<LdrRegister>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")));
    void* cookie = nullptr;
    const LONG status =
        registerNotification != nullptr ? registerNotification(0, &OnDllNotification, nullptr, &cookie) : -1;
    if (status != 0)
    {
        Log("could not register for DLL load notifications (0x%08lX); the game runs without interception",
            static_cast<unsigned long>(status));
        return false;
    }
    g_started = true;

    // The core's export table will lead into this DLL: it must stay loaded as long as the process.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&NgxHookStart), &self))
        Log("could not pin dxgi.dll in the process (%lu)", GetLastError());
    Log("watching for %ls", kCoreName);

    // Normally the game loads the core long after us. If it is already here, anything that took its addresses before
    // now keeps calling the core directly; the log says so, and the calls it still makes through us show how much.
    const HMODULE core = GetModuleHandleW(kCoreName);
    if (core != nullptr)
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(core, path, MAX_PATH);
        char path8[MAX_PATH * 3];
        Utf8(path, int(length), path8, int(std::size(path8)));
        Log("core was already loaded when dxgi.dll started: %s at %p; addresses taken before now are not intercepted",
            path8, static_cast<void*>(core));
        PatchCore(reinterpret_cast<BYTE*>(core), path8);
    }
    return true;
}

HMODULE NgxCoreModule()
{
    return reinterpret_cast<HMODULE>(const_cast<BYTE*>(g_coreModule.load(std::memory_order_acquire)));
}

void NgxHookStop()
{
    if (!g_started)
        return;
    char line[768];
    const size_t length = DescribeTotals(line, sizeof line);
    LogAtExit("process ending; totals: %s", length != 0 ? line : "no DLSS calls");
}
