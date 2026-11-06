// coop/dev/director/blocker_delete.h -- the director's answer to a prop on its route. A walker stopped by a box in a
// doorway deletes the box, the drill's own object deleter. Dev only, reached only from the director's Goto, on the
// game thread.
//
// A drill's world is the rig's own: the rig starts each run from its seeded save or a New Game, so a deleted prop
// never persists into the next run. The delete is the game's own destroy (ue_wrap::engine::DestroyActor), which the
// destroy seam sees (coop/props/prop_destroy_seam.cpp): the host broadcasts PropDestroy, and a client's destroy
// makes the host destroy its copy by key or eid (coop/dispatch/event_dispatch_entity.cpp), so no peer keeps the
// prop. The seam holds that back inside the world-load and reconcile window, and for a child actor or a prop with no
// key, so the delete refuses each of those. DestroyLocalProp is never used: it marks the destroy as incoming and
// nothing is sent.
//
// Off with VOTVCOOP_DRILL_DELETE=0 (a run that measures the walk against its props); not a config row. At most six
// deletes per process, counted across every goal the process runs, never reset.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::director {

// A drill's walker stopped by a prop on its route deletes the prop through the game's own destroy (dev only).
// False: the delete is off, the actor is not a keyed prop, or the per-process bound is spent -- the caller goes on
// to its detour. `at` is the leg's hit point and `why` names the leg in the log line.
bool DeleteBlocker(void* player, void* prop, const ue_wrap::FVector& at, const char* why);

// True when VOTVCOOP_DRILL_DELETE=0, so a false from DeleteBlocker means "off" and not "keyless prop" or "bound
// spent". A goal that meets a blocker it was told not to remove fails on it at once.
bool BlockerDeleteOff();

}  // namespace coop::director
