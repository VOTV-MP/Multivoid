// coop/props/prop_lifecycle_detail.h -- INTERNAL shared state + seam entry
// points between prop_lifecycle.cpp (spawn-catch observers, Install),
// prop_destroy_seam.cpp (the K2_DestroyActor Func-patch seam + explicit
// destroy API), and prop_container_extract.cpp (the propInventory takeObj
// PRE/POST seam + InstallInventory), extracted 2026-07-10 when
// prop_lifecycle passed the 800-LOC soft cap (two slices).
//
// Sibling-internal header (the session_lanes.h / event_dispatch.h precedent)
// -- NOT part of the public coop/ include surface; only the three TUs above
// include it.

#pragma once

#include "coop/net/session.h"

#include <atomic>

namespace coop::prop_lifecycle {

// Cached session pointer (set on Install/InstallInventory via SetSession).
// Atomic: observers fire from parallel-anim worker threads (audit C2
// 2026-05-27); defined in prop_lifecycle.cpp.
extern std::atomic<coop::net::Session*> g_session_ptr;

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// takeObj-in-flight bracket (defined in prop_container_extract.cpp, set by
// the takeObj PRE/POST pair): the nested Aprop_C::Init POST observer in
// prop_lifecycle.cpp defers its broadcast while true (Key is NewGuid
// pre-loadData; the takeObj POST is the canonical broadcaster). Relaxed
// order suffices -- see the definition's audit-H12 comment.
extern std::atomic<bool> g_takeObjInFlight;

// The destroy seam (prop_destroy_seam.cpp). OnK2DestroyFunc is the
// ufunction_hook Func-patch callback prop_lifecycle::Install registers on
// Actor.K2_DestroyActor; DestroySeamBody is its body (the dying actor is the
// dispatch context).
void DestroySeamBody(void* self);
void OnK2DestroyFunc(void* context, void* srcObj, void* result);

}  // namespace coop::prop_lifecycle
