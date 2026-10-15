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

// {6B616E61-6E61-4C69-7374-537461746521}: on a list once a Reset or ClearState of it went through us, from when its
// slot's state is its own. A list created at the address of one destroyed before carries none until its own Reset, so
// the old list's state, kept in the same slot, is not taken for its own. It used to be set at the list's first NR
// pass, which was then skipped: GTA V records DLSS on lists of a pool of about a thousand (1,000 passes skipped in
// eight minutes, fewer as they went), and each skip was a frame of the game's own picture between NR's, brighter on
// walls and pale surfaces: a flash every few seconds.
const GUID kSeen = { 0x6b616e61, 0x6e61, 0x4c69, { 0x73, 0x74, 0x53, 0x74, 0x61, 0x74, 0x65, 0x21 } };

// One watched function table, and what its watched entries held before we took them. `vtable` is set last, once
// `original` is complete; a slot is never given back. `complete` once every watched entry is ours: the entries are
// written one by one (two VirtualProtect calls each, up to 0.1 ms in all), and a Reset that reaches us before the
// binding entries are ours vouches for nothing (the binds right after it are not seen).
struct Table
{
    std::atomic<void**> vtable;
    void* original[kWatchedCount];
    std::atomic<bool> complete;
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
std::atomic<unsigned> g_slotsTaken{ 0 }; // for the log
std::atomic<uint64_t> g_resets{ 0 }; // Resets and ClearStates through our hooks
std::atomic<uint64_t> g_made{ 0 };   // lists the device made, cleaned at their birth
// The reasons a list's state became unknown, each logged once.
std::atomic<bool> g_saidBundle{ false };
std::atomic<bool> g_saidOverflow{ false };
std::atomic<bool> g_saidTableChanged{ false };
std::atomic<bool> g_saidBadHeaps{ false };
std::atomic<bool> g_saidBadConstants{ false };

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
        if (slot.list.compare_exchange_strong(held, list, std::memory_order_acq_rel))
        {
            g_slotsTaken.fetch_add(1, std::memory_order_relaxed);
            return &slot.state;
        }
        if (held == list)
            return &slot.state;
    }
    if (claim && !g_full.exchange(true, std::memory_order_relaxed))
        Log("NR: more than %u command lists seen; the state of further lists is not followed", kSlots);
    return nullptr;
}

void Unknown(ListState* s, std::atomic<bool>* said, const char* why)
{
    s->known = false;
    s->lost = why;
    if (said != nullptr && !said->exchange(true, std::memory_order_relaxed))
        Log("NR: a command list's state is not known: %s; NR waits for its next Reset (logged once)", why);
}

// Whether `list` carries our mark for itself: the mark holds the list's own pointer, as its slot is keyed by it, so a
// mark written through a wrapper or the object inside one vouches for nothing on the other.
bool Seen(ID3D12GraphicsCommandList* list)
{
    void* marked = nullptr;
    UINT size = sizeof marked;
    return SUCCEEDED(list->GetPrivateData(kSeen, &size, &marked)) && size == sizeof marked && marked == list;
}

// After a Reset or ClearState (or the list's creation): nothing bound but `pipeline`, and the list marked (about
// 11 ns when it is already, against some 65 us for a Close and Reset). Known only on a table that is all ours: the
// runtime's Reset also puts a list back on its class's table, which may not be taken yet, and while a table is being
// taken its binding entries are still the runtime's.
void Clean(ListState* s, ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline, bool made = false)
{
    *s = {};
    void** const vtable = *reinterpret_cast<void** const*>(list);
    const Table* table = FindTable(vtable);
    s->known = table != nullptr && table->complete.load(std::memory_order_acquire);
    if (!s->known)
        s->lost = made ? "it was made on a function table not (yet) all ours, so what it bound after was not all seen"
                       : "it was reset on a function table not (yet) all ours, so what it bound after was not all seen";
    s->pipeline = pipeline;
    s->table = vtable;
    if (!Seen(list))
    {
        const void* const self = list;
        list->SetPrivateData(kSeen, sizeof self, &self);
    }
}

void SetArg(ID3D12GraphicsCommandList* list, UINT parameter, ArgKind kind, uint64_t value)
{
    ListState* s = StateOf(list, true);
    if (s == nullptr)
        return;
    if (parameter >= kListStateArgs)
    {
        Unknown(s, &g_saidOverflow, "a root parameter beyond 64 was set");
        return;
    }
    s->args[parameter] = { kind, value };
}

void SetConstant(ListState* s, UINT parameter, UINT offset, uint32_t value)
{
    if (parameter >= kListStateArgs || offset >= kListStateConstants)
    {
        Unknown(s, &g_saidOverflow, "a root constant beyond 64 was set");
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
        Unknown(s, &g_saidOverflow, "more than 64 root constants were set");
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
        g_resets.fetch_add(1, std::memory_order_relaxed);
        if (ListState* s = StateOf(list, true))
            Clean(s, list, pipeline);
    }
    return result;
}

void STDMETHODCALLTYPE HookClearState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    typedef void(STDMETHODCALLTYPE * Fn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    Original<Fn, kClearState>(list)(list, pipeline);
    g_resets.fetch_add(1, std::memory_order_relaxed);
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
        Unknown(s, &g_saidBundle, "a bundle was executed on it (ExecuteBundle)");
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
            // Not a call that succeeds; the heaps are now anyone's guess.
            Unknown(s, &g_saidBadHeaps, "SetDescriptorHeaps was given more than two heaps, or none");
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
            Unknown(s, &g_saidBadConstants, "SetComputeRoot32BitConstants was given no values");
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

// ---------------------------------------------------------------------------------------------------------------
// Lists made in the recording state. CreateCommandList hands back a list already recording, as after a Reset with its
// initial pipeline but without one, so such a list was unknown at its first NR pass: a frame without NR. GTA V brought
// new lists all through a session, a skipped frame every few seconds even after its pool of about a thousand had all
// been seen. The device's CreateCommandList is taken with the first table, and a list it makes is cleaned and marked at
// once (its state is known from its birth). CreateCommandList1 makes closed lists, which need a Reset anyway.

constexpr unsigned kCreateCommandList = 12; // in ID3D12Device's table: IUnknown's 3, ID3D12Object's 4, then 5 more
std::atomic<void*> g_createOriginal{ nullptr };

HRESULT STDMETHODCALLTYPE HookCreateCommandList(ID3D12Device* device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial,
                                                REFIID riid, void** made)
{
    typedef HRESULT(STDMETHODCALLTYPE * Fn)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                            ID3D12PipelineState*, REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(g_createOriginal.load(std::memory_order_acquire))(
        device, nodeMask, type, allocator, initial, riid, made);
    if (SUCCEEDED(result) && made != nullptr && *made != nullptr &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE))
    {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*made)->QueryInterface(IID_PPV_ARGS(&list))))
        {
            // Only on a table we follow in full: a list on another (D3D12Core gives COMPUTE lists a table of their
            // own, taken only once one carries DLSS) would be unknown all the same, and is not worth a slot.
            const Table* table = FindTable(*reinterpret_cast<void***>(list));
            if (table != nullptr && table->complete.load(std::memory_order_acquire))
            {
                g_made.fetch_add(1, std::memory_order_relaxed);
                if (ListState* s = StateOf(list, true))
                    Clean(s, list, initial, true);
            }
            list->Release();
        }
    }
    return result;
}

// Once, with the first table, under the NR lock.
void WatchCreation(ID3D12GraphicsCommandList* list)
{
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        Log("NR: the command list gave no device; lists made from now on are not followed from their creation");
        return;
    }
    void** const table = *reinterpret_cast<void***>(device);
    void* const entry = table[kCreateCommandList];
    char where[160];
    Where(entry, true, where, sizeof where);
    if (entry != reinterpret_cast<void*>(&HookCreateCommandList))
    {
        g_createOriginal.store(entry, std::memory_order_release); // before any call can reach the hook
        if (WriteEntry(&table[kCreateCommandList], reinterpret_cast<void*>(&HookCreateCommandList)))
            Log("NR: command lists made from now on are followed from their creation (the device's CreateCommandList, "
                "%s)",
                where);
        else
            Log("NR: the device's CreateCommandList (%s) could not be taken (error %lu); a list made in the recording "
                "state is not known until its first Reset",
                where, GetLastError());
    }
    device->Release();
}

// Takes the watched entries of `list`'s table, once per table. Callers hold the NR lock, so two never race here.
bool Watch(ID3D12GraphicsCommandList* list)
{
    void** const vtable = *reinterpret_cast<void***>(list);
    if (const Table* known = FindTable(vtable))
        return known->complete.load(std::memory_order_acquire);
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
    table->complete.store(taken == kWatchedCount, std::memory_order_release);
    if (first == nullptr)
        WatchCreation(list);
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

bool ListStateCapture(ID3D12GraphicsCommandList* list, ListState* out, const char** why)
{
    const char* unused = nullptr;
    const char*& reason = why != nullptr ? *why : unused;
    out->known = false;
    reason = "no command list";
    if (list == nullptr)
        return false;
    if (!Watch(list))
    {
        reason = "its function table is not followed (see the log above)";
        return false;
    }
    ListState* s = StateOf(list, false);
    if (s == nullptr)
    {
        reason = g_full.load(std::memory_order_relaxed)
                     ? "it has no slot: more command lists than are followed"
                     : "no Reset of it went through us yet: recording since before the watch, made in the recording "
                       "state, or born where a followed list died";
        return false;
    }
    if (!Seen(list))
    {
        // No Reset of it went through us: recording since before we watched, made in the recording state, or born
        // where a list we saw died, and what the slot says may be the old list's.
        reason = "no Reset of it went through us yet: recording since before the watch, made in the recording state, "
                 "or born where a followed list died";
        s->known = false;
        return false;
    }
    if (!s->known)
    {
        reason = s->lost != nullptr ? s->lost : "not known since its last Reset";
        return false;
    }
    if (s->table != *reinterpret_cast<void* const*>(list))
    {
        // Given another table since its Reset (by some other hook): what went through that one we did not see.
        Unknown(s, &g_saidTableChanged, "it was given another function table since its Reset");
        reason = s->lost;
        return false;
    }
    *out = *s;
    reason = nullptr;
    return true;
}

void ListStateCount(ListStateCounts* out)
{
    out->lists = g_slotsTaken.load(std::memory_order_relaxed);
    out->resets = g_resets.load(std::memory_order_relaxed);
    out->made = g_made.load(std::memory_order_relaxed);
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
