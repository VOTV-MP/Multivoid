// coop/props/remote_prop_internal.h -- IMPLEMENTATION-PRIVATE shared seam among the
// remote_prop TU family (remote_prop.cpp / remote_prop_destroy.cpp / remote_prop_convert.cpp /
// remote_prop_physics.cpp).
//
// NOT a public header (lives under src/, not include/): it declares the two symbols the
// destroy-path TU (remote_prop_destroy.cpp) and the main receiver TU (remote_prop.cpp)
// must share but that are NOT part of the public remote_prop.h surface. Anti-smear
// extraction 2026-06-30: the prop-DESTROY path (resolve -> clear drive -> release grab ->
// echo-suppress -> K2_DestroyActor, incl. the destroy-before-load deferred re-apply) moved
// to its own TU; these two functions straddle the cut.
//
// Game-thread only (same contract as the rest of remote_prop).

#pragma once

#include <cstdint>

namespace coop::remote_prop {

// v26 resolver: a Prop Element id -> its live mirror actor (UAF-safe IsLiveByIndex). Defined in
// remote_prop.cpp (used there by the drive/release/convert paths); declared here so the destroy TU
// (OnDestroyImpl_) can resolve the non-keyable trash clump by eid. null on miss/dead.
void* ResolveLiveActorByEid(uint32_t eid);

// Echo-suppressed local destroy of `actor`: ClearAnyDriveFor -> (if K2_DestroyActor resolved)
// MarkIncomingDestroy + K2_DestroyActor. Defined in remote_prop_destroy.cpp (it owns the cached
// destroy UFunction); called by remote_prop_convert.cpp's OnConvert to retire the OLD rendering
// after a re-skin rebind, without that TU touching the destroy-fn global. No-op on a dead/null actor.
void DestroyEchoSuppressed(void* actor);

// Fire Aprop_C.thrown(Player) on the prop actor (BP throw sound + particle-trail dispatch).
// Defined in remote_prop_physics.cpp (s28 cut -- it owns the cached Aprop_C.thrown resolve state);
// called by remote_prop.cpp's OnRelease on the speed-gated throw edge. Skips silently on a null
// actor/player or an unresolved fn.
void DrivePropThrown(void* propActor, void* localPlayer);

}  // namespace coop::remote_prop
