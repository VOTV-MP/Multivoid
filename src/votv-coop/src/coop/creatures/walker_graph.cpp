// coop/creatures/walker_graph.cpp -- see coop/creatures/walker_graph.h.

#include "coop/creatures/walker_graph.h"

#include "ue_wrap/core/script_calls.h"

#include <vector>

namespace coop::walker_graph {

namespace SC = ue_wrap::script_calls;

bool FindCalls(void* graph, void* native, Calls& out) {
    out = Calls{};
    std::vector<SC::CallSite> sites;
    if (!graph || !native || !SC::EnumerateCalls(graph, sites)) return false;
    for (const SC::CallSite& site : sites) {
        if (!SC::SameCallee(site, native)) continue;
        if (out.found < kMaxCalls) out.ends[out.found] = site.end;
        ++out.found;
    }
    return true;
}

}  // namespace coop::walker_graph
