// coop/props/prop_lifecycle_detail.h -- INTERNAL shared state and seam entry points between
// prop_lifecycle.cpp (spawn-catch observers, Install), prop_destroy_seam.cpp (the
// K2_DestroyActor Func-patch seam and the explicit destroy API).
// The two were cut apart when prop_lifecycle passed the 800-line soft cap.
//
// Sibling-internal header (the session_lanes.h / event_dispatch.h precedent) -- NOT part of
// the public coop/ include surface; only the two translation units above include it.

#pragma once

#include "coop/net/session.h"
#include "ue_wrap/core/script_gate.h"  // Verdict

#include <atomic>
#include <cstdint>

namespace coop::prop_lifecycle {

// Cached session pointer (set on Install via SetSession). Atomic because the
// observers fire from parallel-anim worker threads; defined in prop_lifecycle.cpp.
extern std::atomic<coop::net::Session*> g_session_ptr;

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// The destroy seam (prop_destroy_seam.cpp). OnK2DestroyFunc is the
// ufunction_hook Func-patch callback prop_lifecycle::Install registers on
// Actor.K2_DestroyActor; DestroySeamBody is its body (the dying actor is the
// dispatch context). OnK2DestroyPre is the pre hook on the same function: it records the local
// hand-view actor at the call's entry, and the post side passes what the record said to the body.
enum class HandView : uint8_t { No, Nested, Outermost };
void DestroySeamBody(void* self, HandView handView);
void OnK2DestroyFunc(void* context, void* srcObj, void* result);
ue_wrap::script_gate::Verdict OnK2DestroyPre(void* context, void* srcObj, const uint8_t* parms);

}  // namespace coop::prop_lifecycle
