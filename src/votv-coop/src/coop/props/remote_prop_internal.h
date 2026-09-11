// coop/props/remote_prop_internal.h -- IMPLEMENTATION-PRIVATE shared seam among the
// remote_prop TU family (remote_prop.cpp / remote_prop_destroy.cpp / remote_prop_convert.cpp /
// remote_prop_physics.cpp).
//
// NOT a public header (it lives under src/, not include/): it declares the two symbols those
// TUs share across the cuts that split them, and that are NOT part of the public remote_prop.h
// surface. Each is defined in the TU owning the cached state it needs, and called from
// another.
//
// Game-thread only (same contract as the rest of remote_prop).

#pragma once

#include <cstdint>

namespace coop::remote_prop {

// Echo-suppressed local destroy of `actor`: ClearAnyDriveFor, then (if K2_DestroyActor
// resolved) MarkIncomingDestroy and K2_DestroyActor. Defined in remote_prop_destroy.cpp, which
// owns the cached destroy UFunction; called by remote_prop_convert.cpp's OnConvert to retire
// the OLD rendering after a re-skin rebind, without that TU touching the destroy-fn global.
// No-op on a dead or null actor.
void DestroyEchoSuppressed(void* actor);

// Fire Aprop_C.thrown(Player) on the prop actor (BP throw sound + particle-trail dispatch).
// Defined in remote_prop_physics.cpp, which owns the cached Aprop_C.thrown resolve state;
// called by remote_prop.cpp's OnRelease on the speed-gated throw edge. Skips silently on a
// null actor or player, or an unresolved fn.
void DrivePropThrown(void* propActor, void* localPlayer);

}  // namespace coop::remote_prop
