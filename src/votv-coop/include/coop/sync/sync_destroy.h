// coop/sync/sync_destroy.h -- RetireMirror, the ONE type-dispatched mirror destroy funnel.
//
// The symmetric counterpart to sync_create's CreateOrAdopt: where create owns the
// "which manager Install"s a wire mirror, RetireMirror owns the "which manager
// Take"s it and hands the drained Element to the deferred ElementDeleter. The
// destroy CONTRACT was already ElementDeleter (Enqueue any-thread -> Flush GT at
// net_pump top-of-tick); this funnel removes the remaining per-producer
// `ElementDeleter::Enqueue(MirrorManager<T>::Take(eid))` boilerplate so the
// type-dispatch lives in ONE place keyed by the Element's own ElementType, not
// smeared across npc_sync / world_actor_sync / kerfur_reconcile each hard-coding
// their T. MTA analog: CElementDeleter::Delete(pEntity) (one remove path).
//
// Game-thread for the Take (touches the manager + registry under their mutexes);
// the Enqueue is any-thread-safe. A bare-actor retire (proxy un-root, echo-
// suppress) keeps its site-specific pre-steps -- this funnel is only the
// Element teardown.

#pragma once

#include "coop/element/element.h"  // ElementId

namespace coop::sync {

// Retire the wire mirror Element bound at `eid`: resolve its ElementType, Take it
// from the matching MirrorManager, and Enqueue it on the deferred ElementDeleter
// (GC-safe teardown at the next net_pump Flush). No-op if `eid` is unbound or its
// type is not a streamed mirror kind (Player/Kerfur/Unknown are not retired here).
void RetireMirror(coop::element::ElementId eid);

}  // namespace coop::sync
