// coop/creatures/walker_graph.h -- where a native is called in the walking mannequin's event graph.
//
// The walker's Blueprint runs every entry through one function, ExecuteUbergraph_prop_wMannequin. A native post hook that must act
// only at the walker's own calls of a native (the camera manager read, the gaze reads, the damping write) names each call by the
// offset it ends at (ufunction_hook::CallerFrame::codeOffset), found here from the bytecode and never written down, so a build
// that moved them is read afresh. Coop layer; game thread.

#pragma once

#include <cstdint>

namespace coop::walker_graph {

// The most call ends kept; a graph holding more calls of the native than this reports the whole count in `found`.
inline constexpr int kMaxCalls = 4;

struct Calls {
    int32_t ends[kMaxCalls] = {-1, -1, -1, -1};
    int     found = 0;  // every call of the native in the graph, which may exceed kMaxCalls
};

// The end offsets of `graph`'s calls of `native`, in the bytecode's own order. False when the graph or the native is null or the
// graph does not decode (`out` is then empty). The decode reads the whole class's script: ask once per graph, keep the result.
bool FindCalls(void* graph, void* native, Calls& out);

}  // namespace coop::walker_graph
