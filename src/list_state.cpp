// The game's compute state per command list: see list_state.h.

#include "list_state.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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
    kSetPipelineState1 = 75
};

constexpr Index kWatched[] = { kReset,
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
constexpr unsigned kWatchedCount = unsigned(std::size(kWatched));

// Where an entry's original is kept in a Table.
constexpr unsigned Position(Index index)
{
    for (unsigned i = 0; i < kWatchedCount; ++i)
    {
        if (kWatched[i] == index)
            return i;
    }
    return kWatchedCount;
}

enum ArgKind : uint8_t
{
    kArgNone,
    kArgTable,
    kArgCbv,
    kArgSrv,
    kArgUav
};

// The function tables we watch. Most games have one per list class, in the D3D12 runtime. In Gears of War E-Day the
// lists came with tables outside any module, a new one on each of the first four frames, and with room for four the
// NR pass followed only the lists that happened to have one of those (941 of 37,719 frames). So they are kept in a
// hash, as many as come.
constexpr unsigned kTableBits = 12;
constexpr unsigned kTableSlots = 1u << kTableBits;
constexpr unsigned kTablesMax = kTableSlots / 4 * 3; // past this a list with a new table is not followed
constexpr unsigned kSlotBits = 11;
constexpr unsigned kSlots = 1u << kSlotBits; // lists followed; a slot is never given back

// {6B616E61-6E61-4C69-7374-537461746521}: on a list once the NR pass has seen it. A list created at the address of
// one destroyed before carries none, so the old list's state, kept in the same slot, is not taken for its own.
const GUID kSeen = { 0x6b616e61, 0x6e61, 0x4c69, { 0x73, 0x74, 0x53, 0x74, 0x61, 0x74, 0x65, 0x21 } };

// One watched function table, and what its watched entries held before we took them. `vtable` is set last, once
// `original` is complete; a slot is never given back.
struct Table
{
    std::atomic<void**> vtable;
    void* original[kWatchedCount];
};

Table g_tables[kTableSlots];
std::atomic<const Table*> g_firstTable{ nullptr };
unsigned g_tableCount = 0;   // under the NR lock, as everything Watch does
unsigned g_tablesAstray = 0; // tables whose entries lead elsewhere than the first one's
bool g_saidTablesFull = false;
bool g_saidCopied = false;

uint64_t TableHash(const void* vtable) { return (uint64_t(uintptr_t(vtable)) >> 3) * 0x9E3779B97F4A7C15ull; }

// The watched table `vtable`, or null. Any thread: the hooks call it on every call they see.
const Table* FindTable(void** vtable)
{
    const unsigned start = unsigned(TableHash(vtable) >> (64 - kTableBits));
    for (unsigned i = 0; i < kTableSlots; ++i)
    {
        const Table& table = g_tables[(start + i) & (kTableSlots - 1)];
        void** const held = table.vtable.load(std::memory_order_acquire);
        if (held == vtable)
            return &table;
        if (held == nullptr)
            return nullptr;
    }
    return nullptr;
}

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
std::atomic<bool> g_saidTableChanged{ false };

template <typename F, Index I> F Original(ID3D12GraphicsCommandList* list)
{
    static_assert(Position(I) < kWatchedCount, "not a watched entry");
    const Table* table = FindTable(*reinterpret_cast<void***>(list));
    // Reached through a table we did not take (another hook copied our entry): the first table's functions are the
    // class's own as far as we know.
    if (table == nullptr)
        table = g_firstTable.load(std::memory_order_acquire);
    return reinterpret_cast<F>(table->original[Position(I)]);
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

void Clean(ListState* s, ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    *s = {};
    s->known = true;
    s->pipeline = pipeline;
    s->table = *reinterpret_cast<void* const*>(list);
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
    const HRESULT result = Original<Fn, kReset>(list)(list, allocator, pipeline);
    if (SUCCEEDED(result))
    {
        if (ListState* s = StateOf(list, true))
            Clean(s, list, pipeline);
    }
    return result;
}

void STDMETHODCALLTYPE HookClearState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    Original<Fn, kClearState>(list)(list, pipeline);
    if (ListState* s = StateOf(list, true))
        Clean(s, list, pipeline);
}

void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    if (ListState* s = StateOf(list, true))
    {
        s->pipeline = pipeline;
        s->stateObject = nullptr; // the two replace each other
    }
    Original<Fn, kSetPipelineState>(list)(list, pipeline);
}

void STDMETHODCALLTYPE HookExecuteBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
    // What the bundle binds stays bound in the list afterwards, and we do not follow bundles.
    if (ListState* s = StateOf(list, true))
        Unknown(s, &g_saidBundle, "ExecuteBundle");
    Original<Fn, kExecuteBundle>(list)(list, bundle);
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
    Original<Fn, kSetDescriptorHeaps>(list)(list, count, heaps);
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
    Original<Fn, kSetComputeRootSignature>(list)(list, root);
}

void STDMETHODCALLTYPE HookSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* list, UINT parameter,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    SetArg(list, parameter, kArgTable, table.ptr);
    Original<Fn, kSetComputeRootDescriptorTable>(list)(list, parameter, table);
}

void STDMETHODCALLTYPE HookSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* list, UINT parameter, UINT value,
                                                      UINT offset)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
    if (ListState* s = StateOf(list, true))
        SetConstant(s, parameter, offset, value);
    Original<Fn, kSetComputeRoot32BitConstant>(list)(list, parameter, value, offset);
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
    Original<Fn, kSetComputeRoot32BitConstants>(list)(list, parameter, count, values, offset);
}

void STDMETHODCALLTYPE HookSetComputeRootConstantBufferView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgCbv, address);
    Original<Fn, kSetComputeRootConstantBufferView>(list)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetComputeRootShaderResourceView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgSrv, address);
    Original<Fn, kSetComputeRootShaderResourceView>(list)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* list, UINT parameter,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    SetArg(list, parameter, kArgUav, address);
    Original<Fn, kSetComputeRootUnorderedAccessView>(list)(list, parameter, address);
}

void STDMETHODCALLTYPE HookSetPipelineState1(ID3D12GraphicsCommandList* list, ID3D12StateObject* stateObject)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12StateObject*);
    if (ListState* s = StateOf(list, true))
    {
        s->stateObject = stateObject;
        s->pipeline = nullptr; // the two replace each other
    }
    Original<Fn, kSetPipelineState1>(list)(list, stateObject);
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

// The module holding `address` ("D3D12Core.dll"), with the offset in it when `offset` is given; else what memory it
// is in. For the log.
void Where(const void* address, bool offset, char* out, size_t size)
{
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner))
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(owner, path, MAX_PATH);
        const wchar_t* name = L"a module without a name";
        if (length != 0 && length < MAX_PATH)
        {
            const wchar_t* slash = wcsrchr(path, L'\\');
            name = slash != nullptr ? slash + 1 : path;
        }
        if (offset)
            std::snprintf(out, size, "%ls+0x%llX", name,
                          static_cast<unsigned long long>(uintptr_t(address) - uintptr_t(owner)));
        else
            std::snprintf(out, size, "%ls", name);
        return;
    }
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof info) != sizeof info)
    {
        std::snprintf(out, size, "no module (%p)", address);
        return;
    }
    const char* type = info.Type == MEM_PRIVATE ? "private" : info.Type == MEM_MAPPED ? "mapped" : "image";
    if (offset)
        std::snprintf(out, size, "no module (%p, %s memory allocated at %p)", address, type, info.AllocationBase);
    else
        std::snprintf(out, size, "no module (%s memory)", type);
}

// Where a table's originals lead, module by module: "D3D12Core.dll 13". For the log.
void DescribeOriginals(const Table& table, char* out, size_t size)
{
    char names[4][96] = {};
    unsigned counts[4] = {};
    unsigned distinct = 0, others = 0;
    for (void* original : table.original)
    {
        char name[96];
        Where(original, false, name, sizeof name);
        unsigned i = 0;
        while (i < distinct && std::strcmp(names[i], name) != 0)
            ++i;
        if (i == distinct && distinct == std::size(names))
        {
            ++others;
            continue;
        }
        if (i == distinct)
            std::snprintf(names[distinct++], sizeof names[0], "%s", name);
        ++counts[i];
    }
    int at = 0;
    for (unsigned i = 0; i < distinct && at >= 0 && size_t(at) < size; ++i)
        at += std::snprintf(out + at, size - size_t(at), "%s%s %u", i != 0 ? ", " : "", names[i], counts[i]);
    if (others != 0 && at >= 0 && size_t(at) < size)
        std::snprintf(out + at, size - size_t(at), ", elsewhere %u", others);
}

// Takes the watched entries of `list`'s table, once per table. Callers hold the NR lock, so two never race here.
bool Watch(ID3D12GraphicsCommandList* list)
{
    void** const vtable = *reinterpret_cast<void***>(list);
    if (FindTable(vtable) != nullptr)
        return true;
    if (g_tableCount == kTablesMax)
    {
        if (!g_saidTablesFull)
        {
            g_saidTablesFull = true;
            Log("NR: %u command list tables followed; a list with yet another one is not followed", kTablesMax);
        }
        return false;
    }
    // SetPipelineState1 is ID3D12GraphicsCommandList4's: a runtime without it has a shorter table.
    ID3D12GraphicsCommandList4* four = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&four))))
    {
        static bool said = false;
        if (!said)
            Log("NR: the game's command list has no ID3D12GraphicsCommandList4; its state cannot be followed");
        said = true;
        return false;
    }
    four->Release();
    Table* table = nullptr;
    const unsigned start = unsigned(TableHash(vtable) >> (64 - kTableBits));
    for (unsigned i = 0; i < kTableSlots && table == nullptr; ++i)
    {
        Table& candidate = g_tables[(start + i) & (kTableSlots - 1)];
        if (candidate.vtable.load(std::memory_order_relaxed) == nullptr)
            table = &candidate;
    }
    if (table == nullptr)
        return false; // not below kTablesMax

    // An entry that is ours already: the table is a copy of one we took, made after we took it, and taking our entry
    // for the original would call ourselves for ever. The first table's original is the class's own.
    const Table* first = g_firstTable.load(std::memory_order_relaxed);
    unsigned copied = 0;
    bool astray = false;
    for (unsigned i = 0; i < kWatchedCount; ++i)
    {
        void* entry = vtable[kWatched[i]];
        if (entry == HookFor(kWatched[i]))
        {
            if (first == nullptr)
                return false; // cannot be: our entries are only in tables we took
            entry = first->original[i];
            ++copied;
        }
        table->original[i] = entry;
        astray = astray || (first != nullptr && entry != first->original[i]);
    }
    table->vtable.store(vtable, std::memory_order_release); // published before any list can reach our hooks
    ++g_tableCount;
    if (first == nullptr)
        g_firstTable.store(table, std::memory_order_release);
    if (astray)
        ++g_tablesAstray;

    unsigned taken = 0;
    for (Index index : kWatched)
    {
        if (vtable[index] == HookFor(index) || WriteEntry(&vtable[index], HookFor(index)))
            ++taken;
    }
    // The first four one by one, as before there were more, and where their entries lead; then every power of two.
    if (g_tableCount <= 4)
    {
        char where[160];
        char leads[400] = "";
        Where(vtable, true, where, sizeof where);
        DescribeOriginals(*table, leads, sizeof leads);
        Log("NR: following the game's compute state: %u of %u entries of the command list table at %s; they led to "
            "%s%s",
            taken, kWatchedCount, where, leads, astray ? ", not all where the first table's did" : "");
    }
    else if ((g_tableCount & (g_tableCount - 1)) == 0)
        Log("NR: following the game's compute state on %u command list tables; %u of them lead elsewhere than the "
            "first",
            g_tableCount, g_tablesAstray);
    if (copied != 0 && !g_saidCopied)
    {
        g_saidCopied = true;
        Log("NR: a command list table held %u of our entries already: a copy of one we took, its originals are the "
            "first table's (logged once)",
            copied);
    }
    return taken == kWatchedCount;
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
    if (s->table != *reinterpret_cast<void* const*>(list))
    {
        // Given another table since its Reset (by some other hook): what went through that one we did not see.
        Unknown(s, &g_saidTableChanged, "it was given another function table");
        return false;
    }
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
