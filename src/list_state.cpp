// The game's compute state per command list: see list_state.h.

#include "list_state.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <iterator>

#include "log.h"

namespace
{
// ID3D12GraphicsCommandList4's function table, as d3d12.h lays it out.
enum Index : unsigned
{
    kReset = 10,
    kClearState = 11,
    kSetPipelineState = 25,
    kExecuteBundle = 27,
    kSetDescriptorHeaps = 28,
    kSetComputeRootSignature = 29,
    kSetComputeRootDescriptorTable = 31,
    kSetComputeRoot32BitConstant = 33,
    kSetComputeRoot32BitConstants = 35,
    kSetComputeRootConstantBufferView = 37,
    kSetComputeRootShaderResourceView = 39,
    kSetComputeRootUnorderedAccessView = 41,
    kSetPipelineState1 = 75,
    kEntries = 76
};

const Index kWatched[] = { kReset,
                           kClearState,
                           kSetPipelineState,
                           kExecuteBundle,
                           kSetDescriptorHeaps,
                           kSetComputeRootSignature,
                           kSetComputeRootDescriptorTable,
                           kSetComputeRoot32BitConstant,
                           kSetComputeRoot32BitConstants,
                           kSetComputeRootConstantBufferView,
                           kSetComputeRootShaderResourceView,
                           kSetComputeRootUnorderedAccessView,
                           kSetPipelineState1 };

enum ArgKind : uint8_t
{
    kArgNone,
    kArgTable,
    kArgCbv,
    kArgSrv,
    kArgUav
};

constexpr unsigned kTables = 4;  // distinct list classes we watch
constexpr unsigned kSlotBits = 11;
constexpr unsigned kSlots = 1u << kSlotBits; // lists followed; a slot is never given back

// {6B616E61-6E61-4C69-7374-537461746521}: on a list once the NR pass has seen it. A list created at the address of
// one destroyed before carries none, so the old list's state, kept in the same slot, is not taken for its own.
const GUID kSeen = { 0x6b616e61, 0x6e61, 0x4c69, { 0x73, 0x74, 0x53, 0x74, 0x61, 0x74, 0x65, 0x21 } };

// One watched function table, and what its entries held before we took them.
struct Table
{
    void** vtable;
    void* original[kEntries];
};

Table g_tables[kTables];
std::atomic<unsigned> g_tableCount{ 0 }; // entries below it are complete

// A list and what we know of it. The state is only touched by the thread recording that list (a list moves between
// threads only through the game's own synchronisation), so it needs no atomics of its own.
struct Slot
{
    std::atomic<ID3D12GraphicsCommandList*> list;
    ListState state;
};

Slot g_slots[kSlots];
std::atomic<bool> g_full{ false };
// The reasons a list's state became unknown, each logged once.
std::atomic<bool> g_saidBundle{ false };
std::atomic<bool> g_saidOverflow{ false };

template <typename F> F Original(ID3D12GraphicsCommandList* list, Index index)
{
    void** const vtable = *reinterpret_cast<void***>(list);
    const unsigned count = g_tableCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i)
    {
        if (g_tables[i].vtable == vtable)
            return reinterpret_cast<F>(g_tables[i].original[index]);
    }
    // Reached through a table we did not take (another hook copied our entry): the first table's functions are the
    // class's own as far as we know.
    return reinterpret_cast<F>(g_tables[0].original[index]);
}

ListState* StateOf(ID3D12GraphicsCommandList* list, bool claim)
{
    const uint64_t hash = (uint64_t(uintptr_t(list)) >> 4) * 0x9E3779B97F4A7C15ull;
    const unsigned start = unsigned(hash >> (64 - kSlotBits));
    for (unsigned i = 0; i < kSlots; ++i)
    {
        Slot& slot = g_slots[(start + i) & (kSlots - 1)];
        ID3D12GraphicsCommandList* held = slot.list.load(std::memory_order_acquire);
        if (held == list)
            return &slot.state;
        if (held != nullptr)
            continue;
        if (!claim)
            return nullptr;
        if (slot.list.compare_exchange_strong(held, list, std::memory_order_acq_rel) || held == list)
            return &slot.state;
    }
    if (claim && !g_full.exchange(true, std::memory_order_relaxed))
        Log("NR: more than %u command lists seen; the state of further lists is not followed", kSlots);
    return nullptr;
}

void Unknown(ListState* s, std::atomic<bool>* said, const char* why)
{
    s->known = false;
    if (said != nullptr && !said->exchange(true, std::memory_order_relaxed))
        Log("NR: a command list's state is not known after %s; NR waits for its next Reset (logged once)", why);
}

void Clean(ListState* s, ID3D12PipelineState* pipeline)
{
    *s = {};
    s->known = true;
    s->pipeline = pipeline;
}

void SetArg(ID3D12GraphicsCommandList* list, UINT parameter, ArgKind kind, uint64_t value)
{
    ListState* s = StateOf(list, true);
    if (s == nullptr)
        return;
    if (parameter >= kListStateArgs)
    {
        Unknown(s, &g_saidOverflow, "a root parameter beyond 64");
        return;
    }
    s->args[parameter] = { kind, value };
}

void SetConstant(ListState* s, UINT parameter, UINT offset, uint32_t value)
{
    if (parameter >= kListStateArgs || offset >= kListStateConstants)
    {
        Unknown(s, &g_saidOverflow, "a root constant beyond 64");
        return;
    }
    for (unsigned i = 0; i < s->constantCount; ++i)
    {
        ListStateConstant& c = s->constants[i];
        if (c.parameter == parameter && c.offset == offset)
        {
            c.value = value;
            return;
        }
    }
    if (s->constantCount == kListStateConstants)
    {
        Unknown(s, &g_saidOverflow, "more than 64 root constants");
        return;
    }
    s->constants[s->constantCount++] = { uint8_t(parameter), uint8_t(offset), value };
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks: each notes the call, then makes it.

HRESULT STDMETHODCALLTYPE HookReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                    ID3D12PipelineState* pipeline)
{
    typedef HRESULT(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                            ID3D12PipelineState*);
    const HRESULT result = Original<Fn>(list, kReset)(list, allocator, pipeline);
    if (SUCCEEDED(result))
    {
        if (ListState* s = StateOf(list, true))
            Clean(s, pipeline);
    }
    return result;
}

void STDMETHODCALLTYPE HookClearState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    Original<Fn>(list, kClearState)(list, pipeline);
    if (ListState* s = StateOf(list, true))
        Clean(s, pipeline);
}

void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    if (ListState* s = StateOf(list, true))
    {
        s->pipeline = pipeline;
        s->stateObject = nullptr; // the two replace each other
    }
    Original<Fn>(list, kSetPipelineState)(list, pipeline);
}

void STDMETHODCALLTYPE HookExecuteBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
    // What the bundle binds stays bound in the list afterwards, and we do not follow bundles.
    if (ListState* s = StateOf(list, true))
        Unknown(s, &g_saidBundle, "ExecuteBundle");
    Original<Fn>(list, kExecuteBundle)(list, bundle);
}

void STDMETHODCALLTYPE HookSetDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                             ID3D12DescriptorHeap* const* heaps)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
    if (ListState* s = StateOf(list, true))
    {
        if (count > 2 || (count != 0 && heaps == nullptr))
        {
            Unknown(s, nullptr, nullptr); // not a call that succeeds; the heaps are now anyone's guess
        }
        else
        {
            s->heapCount = count;
            s->heaps[0] = count > 0 ? heaps[0] : nullptr;
            s->heaps[1] = count > 1 ? heaps[1] : nullptr;
        }
    }
    Original<Fn>(list, kSetDescriptorHeaps)(list, count, heaps);
}

void STDMETHODCALLTYPE HookSetComputeRootSignature(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
    if (ListState* s = StateOf(list, true))
    {
        if (s->computeRoot != root)
        {
            // A different root signature leaves no argument bound.
            for (ListStateArg& a : s->args)
                a = {};
            s->constantCount = 0;
        }
        s->computeRoot = root;
    }
    Original<Fn>(list, kSetComputeRootSignature)(list, root);
}

void STDMETHODCALLTYPE HookSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* list, UINT parameter,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    SetArg(list, parameter, kArgTable, table.ptr);
    Original<Fn>(list, kSetComputeRootDescriptorTable)(list, parameter, table);
}

void STDMETHODCALLTYPE HookSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* list, UINT parameter, UINT value,
                                                      UINT offset)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
    if (ListState* s = StateOf(list, true))
        SetConstant(s, parameter, offset, value);
    Original<Fn>(list, kSetComputeRoot32BitConstant)(list, parameter, value, offset);
}

void STDMETHODCALLTYPE HookSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* list, UINT parameter, UINT count,
                                                       const void* values, UINT offset)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
    if (ListState* s = StateOf(list, true))
    {
        if (count != 0 && values == nullptr)
        {
            Unknown(s, nullptr, nullptr);
        }
        else
        {
            const uint32_t* dwords = static_cast<const uint32_t*>(values);
            for (UINT i = 0; i < count && s->known; ++i)
                SetConstant(s, parameter, offset + i, dwords[i]);
        }
    }
    Original<Fn>(list, kSetComputeRoot32BitConstants)(list, parameter, count, values, offset);
}

void STDMETHODCALLTYPE HookSetComputeRootConstantBufferView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgCbv, address);
    Original<Fn>(list, kSetComputeRootConstantBufferView)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetComputeRootShaderResourceView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgSrv, address);
    Original<Fn>(list, kSetComputeRootShaderResourceView)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgUav, address);
    Original<Fn>(list, kSetComputeRootUnorderedAccessView)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetPipelineState1(ID3D12GraphicsCommandList* list, ID3D12StateObject* stateObject)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12StateObject*);
    if (ListState* s = StateOf(list, true))
    {
        s->stateObject = stateObject;
        s->pipeline = nullptr; // the two replace each other
    }
    Original<Fn>(list, kSetPipelineState1)(list, stateObject);
}

void* HookFor(Index index)
{
    switch (index)
    {
    case kReset: return reinterpret_cast<void*>(&HookReset);
    case kClearState: return reinterpret_cast<void*>(&HookClearState);
    case kSetPipelineState: return reinterpret_cast<void*>(&HookSetPipelineState);
    case kExecuteBundle: return reinterpret_cast<void*>(&HookExecuteBundle);
    case kSetDescriptorHeaps: return reinterpret_cast<void*>(&HookSetDescriptorHeaps);
    case kSetComputeRootSignature: return reinterpret_cast<void*>(&HookSetComputeRootSignature);
    case kSetComputeRootDescriptorTable: return reinterpret_cast<void*>(&HookSetComputeRootDescriptorTable);
    case kSetComputeRoot32BitConstant: return reinterpret_cast<void*>(&HookSetComputeRoot32BitConstant);
    case kSetComputeRoot32BitConstants: return reinterpret_cast<void*>(&HookSetComputeRoot32BitConstants);
    case kSetComputeRootConstantBufferView: return reinterpret_cast<void*>(&HookSetComputeRootConstantBufferView);
    case kSetComputeRootShaderResourceView: return reinterpret_cast<void*>(&HookSetComputeRootShaderResourceView);
    case kSetComputeRootUnorderedAccessView: return reinterpret_cast<void*>(&HookSetComputeRootUnorderedAccessView);
    case kSetPipelineState1: return reinterpret_cast<void*>(&HookSetPipelineState1);
    default: return nullptr;
    }
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

// Takes the watched entries of `list`'s table, once per table. Callers hold the NR lock, so two never race here.
bool Watch(ID3D12GraphicsCommandList* list)
{
    void** const vtable = *reinterpret_cast<void***>(list);
    const unsigned count = g_tableCount.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i)
    {
        if (g_tables[i].vtable == vtable)
            return true;
    }
    if (count == kTables)
        return false;
    // SetPipelineState1 is ID3D12GraphicsCommandList4's: a runtime without it has a shorter table.
    ID3D12GraphicsCommandList4* four = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&four))))
    {
        Log("NR: the game's command list has no ID3D12GraphicsCommandList4; its state cannot be followed");
        return false;
    }
    four->Release();
    Table& table = g_tables[count];
    table.vtable = vtable;
    for (unsigned i = 0; i < kEntries; ++i)
        table.original[i] = vtable[i];
    g_tableCount.store(count + 1, std::memory_order_release); // published before any list can reach our hooks

    char module[MAX_PATH] = "unknown module";
    uintptr_t offset = 0;
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(vtable), &owner))
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(owner, path, MAX_PATH);
        if (length != 0 && length < MAX_PATH)
        {
            const wchar_t* slash = wcsrchr(path, L'\\');
            std::snprintf(module, sizeof module, "%ls", slash != nullptr ? slash + 1 : path);
        }
        offset = uintptr_t(vtable) - uintptr_t(owner);
    }
    unsigned taken = 0;
    for (Index index : kWatched)
    {
        if (WriteEntry(&vtable[index], HookFor(index)))
            ++taken;
    }
    Log("NR: following the game's compute state: %u of %u entries of the command list table at %s+0x%llX", taken,
        unsigned(std::size(kWatched)), module, static_cast<unsigned long long>(offset));
    return taken == std::size(kWatched);
}
} // namespace

bool ListStateCapture(ID3D12GraphicsCommandList* list, ListState* out)
{
    out->known = false;
    if (list == nullptr || !Watch(list))
        return false;
    ListState* s = StateOf(list, false);
    UINT one = 1;
    UINT size = sizeof one;
    if (FAILED(list->GetPrivateData(kSeen, &size, &one)))
    {
        // New to us, or born where a list we saw died: what the slot says may be the old list's.
        one = 1;
        list->SetPrivateData(kSeen, sizeof one, &one);
        if (s != nullptr)
            s->known = false;
        return false;
    }
    if (s == nullptr || !s->known)
        return false;
    *out = *s;
    return true;
}

void ListStateRestore(ID3D12GraphicsCommandList* list, const ListState& state)
{
    if (state.heapCount != 0)
        list->SetDescriptorHeaps(state.heapCount, state.heaps);
    if (state.computeRoot != nullptr)
    {
        list->SetComputeRootSignature(state.computeRoot);
        for (UINT i = 0; i < kListStateArgs; ++i)
        {
            const ListStateArg& a = state.args[i];
            switch (a.kind)
            {
            case kArgTable: list->SetComputeRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{ a.value }); break;
            case kArgCbv: list->SetComputeRootConstantBufferView(i, a.value); break;
            case kArgSrv: list->SetComputeRootShaderResourceView(i, a.value); break;
            case kArgUav: list->SetComputeRootUnorderedAccessView(i, a.value); break;
            default: break;
            }
        }
        for (unsigned i = 0; i < state.constantCount; ++i)
        {
            const ListStateConstant& c = state.constants[i];
            list->SetComputeRoot32BitConstant(c.parameter, c.value, c.offset);
        }
    }
    if (state.pipeline != nullptr)
    {
        list->SetPipelineState(state.pipeline);
    }
    else if (state.stateObject != nullptr)
    {
        ID3D12GraphicsCommandList4* four = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&four))))
        {
            four->SetPipelineState1(state.stateObject);
            four->Release();
        }
    }
}
