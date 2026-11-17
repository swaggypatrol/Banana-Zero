#pragma once

// The game's compute state on its command list, so that the NR pass can put it back when it is done. D3D12 has no
// way to ask a list what is bound, and the NR pass (ours, and the model's at creation) binds its own descriptor
// heaps, compute root signature, root arguments and pipeline state. The DLSS call itself evidently leaves them alone,
// so a game may bind its compute root signature once and go on after the DLSS call setting only the arguments that
// changed: Resident Evil Requiem does, and crashed inside the driver at its next SetComputeRootDescriptorTable(2),
// on a root signature of ours with two parameters.
//
// How it is learnt: the first time the NR pass is about to record, entries of the list's virtual function table are
// pointed at us, for every list of that class: Reset and ClearState (after which the state is known: nothing bound),
// SetDescriptorHeaps, SetPipelineState and SetPipelineState1, SetComputeRootSignature and the six compute root
// argument setters, SetGraphicsRootSignature and SetGraphicsRootDescriptorTable, and ExecuteBundle (after which the
// list is unknown: a bundle's bindings stay in the list). The graphics side is followed only for its descriptor
// tables: changing the descriptor heaps, as the NR pass does, leaves every table undefined, the game's graphics tables
// too, and a game may draw on after the DLSS call without setting them again. A list
// not reset since then is unknown, and the NR pass waits for the next frame; a list reset since then is known at its
// first NR pass (GTA V records DLSS on lists of a pool of about a thousand), and so is one the device makes since then
// on a table followed by then (its CreateCommandList is taken with the first table: such a list starts recording
// without a Reset). A Reset counts only on a table whose entries are all ours by then. Where lists come with tables
// of their own (as Gears of War E-Day's did), each table is taken when a list with it first comes, and a list that was
// given another table since its Reset is unknown.

#include <windows.h>

#include <d3d12.h>

#include <cstdint>

constexpr unsigned kListStateArgs = 64;      // root parameters: a root signature holds at most 64 DWORDs
constexpr unsigned kListStateConstants = 64; // root constants, the same limit

struct ListStateArg
{
    uint8_t kind;   // 0 not set, else one of ArgKind in list_state.cpp
    uint64_t value; // a GPU descriptor handle or a GPU virtual address
};

struct ListStateConstant
{
    uint8_t parameter;
    uint8_t offset; // in DWORDs
    uint32_t value;
};

// Everything compute the game has bound on one list since its last Reset.
struct ListState
{
    bool known;     // seen since a Reset or ClearState, and every call fitted
    ID3D12DescriptorHeap* heaps[2];
    UINT heapCount; // 0: none bound
    ID3D12PipelineState* pipeline;  // at most one of these two
    ID3D12StateObject* stateObject; // a ray tracing pipeline (SetPipelineState1)
    ID3D12RootSignature* computeRoot;
    ID3D12RootSignature* graphicsRoot;
    uint64_t graphicsTables[kListStateArgs]; // per graphics root parameter, the table's GPU handle; 0 not set
    ListStateArg args[kListStateArgs];
    ListStateConstant constants[kListStateConstants]; // in order of parameter, then offset
    unsigned constantCount;
    const void* table; // the list's function table at its last Reset
    const char* lost;  // why it is not known since that Reset (a call we cannot follow), for the log; else null
};

// Copies what the game has bound on `list` right now into `out`, the first call also installing the watch. False
// while it is not known, with `*why` (when given) saying why, for the log: the caller then records nothing into the
// list. Only on the thread recording `list`.
bool ListStateCapture(ID3D12GraphicsCommandList* list, ListState* out, const char** why = nullptr);

// How much is followed, for the log's periodic lines. Any thread.
struct ListStateCounts
{
    unsigned lists;  // command lists with a slot of their own
    uint64_t resets; // Resets and ClearStates that went through us
    uint64_t made;   // lists the device made on a table we follow, known from their birth
};
void ListStateCount(ListStateCounts* out);

// Binds `state` on `list` again, after the NR pass bound its own: heaps, then the graphics descriptor tables (the heap
// change left them undefined), then the compute root signature, its arguments (each run of constants at consecutive offsets in one call) and the
// pipeline. The rest of the
// graphics state is left alone: the NR pass binds none of it.
void ListStateRestore(ID3D12GraphicsCommandList* list, const ListState& state);
