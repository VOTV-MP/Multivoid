// coop/props/prop_lifecycle_detail.h -- INTERNAL shared state and seam entry points between
// prop_lifecycle.cpp (spawn-catch observers, Install), prop_destroy_seam.cpp (the
// K2_DestroyActor Func-patch seam and the explicit destroy API), and
// prop_container_extract.cpp (the propInventory takeObj PRE/POST seam and InstallInventory).
// The three were cut apart when prop_lifecycle passed the 800-line soft cap.
//
// Sibling-internal header (the session_lanes.h / event_dispatch.h precedent) -- NOT part of
// the public coop/ include surface; only the three translation units above include it.

#pragma once

#include "coop/net/session.h"

#include <atomic>

namespace coop::prop_lifecycle {

// Cached session pointer (set on Install/InstallInventory via SetSession). Atomic because the
// observers fire from parallel-anim worker threads; defined in prop_lifecycle.cpp.
extern std::atomic<coop::net::Session*> g_session_ptr;

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// takeObj-in-flight bracket (defined in prop_container_extract.cpp, set by the takeObj
// PRE/POST pair): the nested Aprop_C::Init POST observer in prop_lifecycle.cpp defers its
// broadcast while true, because an Aprop extracted from a container spawns INSIDE takeObj,
// before loadData has restored its saved Key, so its Key is still a NewGuid and the takeObj
// POST is the canonical broadcaster. It is an atomic because all three observers fire from
// parallel-anim worker threads; relaxed order suffices, because the PRE-then-POST sequencing
// inside one dispatch is preserved by same-thread execution order and no other state depends
// on this bool becoming visible.
extern std::atomic<bool> g_takeObjInFlight;

// The destroy seam (prop_destroy_seam.cpp). OnK2DestroyFunc is the
// ufunction_hook Func-patch callback prop_lifecycle::Install registers on
// Actor.K2_DestroyActor; DestroySeamBody is its body (the dying actor is the
// dispatch context).
void DestroySeamBody(void* self);
void OnK2DestroyFunc(void* context, void* srcObj, void* result);

}  // namespace coop::prop_lifecycle
