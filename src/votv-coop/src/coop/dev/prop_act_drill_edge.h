// coop/dev/prop_act_drill_edge.h -- the edge phase's step table and what its host half (prop_act_drill_edge.cpp) and
// its client half (prop_act_drill_edge_client.cpp) share. Private to the act drill.

#pragma once

#include "coop/dev/prop_act_drill.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::prop_act_drill::detail {

// What the step tests. Window runs first and alone: c1 holds its own announce from the session's start, so the host
// can arm nothing until the window row has released it.
enum class Kind : uint8_t {
    Window,   // a take of a keyed prop of the save, before the announce: cancelled locally, nothing sent
    Before,   // a second request while the first commit is held: the host waits, then serves it after the commit
    Dead,     // the target dies on the host after the grant, before the replay: the client aborts
    Gone,     // the target dies on the host between the replay and the commit: the commit finds it gone
    Second,   // a second take in the tick of the first: cancelled on the client, nothing sent
    Late      // a request whose wait ran out, granted after the first commit: answered with an abort
};

struct EdgeRow {
    Row act;   // the target and c1's act (Window has no target)
    Kind kind;
};

extern const EdgeRow kEdgeRows[];
extern const size_t kEdgeRowCount;

inline bool TwoProps(Kind k) { return k == Kind::Before || k == Kind::Second || k == Kind::Late; }

// The lines the host and c1 say at a step's waypoints.
inline std::string HostLine(const EdgeRow& r, const char* what) { return std::string(kTag) + " host " + r.act.name + " " + what; }
inline std::string ClientLine(const EdgeRow& r, const char* what) { return std::string(kTag) + " c1 " + r.act.name + " " + what; }

inline uint32_t Since(uint32_t now, uint32_t then) { return now - then; }

// The client half's state back to the start (the host half's reset calls it).
void EdgeClientReset();

}  // namespace coop::dev::prop_act_drill::detail
