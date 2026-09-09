// coop/element/identity_destroy.h -- RetireMirror, the ONE type-dispatched mirror destroy funnel.
//
// The symmetric counterpart to CreateOrAdopt: where create owns which manager INSTALLs a wire
// mirror, RetireMirror owns which manager TAKEs it and hands the drained Element to the deferred
// ElementDeleter. The destroy CONTRACT was already ElementDeleter -- Enqueue from any thread, Flush
// on the game thread at net_pump's top-of-tick -- and this funnel removes the remaining
// per-producer Enqueue(Take(eid)) boilerplate, so the type dispatch lives in ONE place keyed by the
// Element's own ElementType instead of smeared across npc_sync, world_actor_sync and
// kerfur_reconcile each hard-coding their own. MTA analog: CElementDeleter::Delete, one remove
// path.
//
// Game-thread for the Take (it touches the manager and the registry under their mutexes); the
// Enqueue is any-thread-safe. A bare-actor retire, a proxy un-root or an echo-suppress, keeps its
// site-specific pre-steps -- this funnel is only the Element teardown.

#pragma once

#include "coop/element/element.h"  // ElementId

namespace coop::element {

// Retire the wire mirror Element bound at `eid`: resolve its ElementType, Take it
// from the matching MirrorManager, and Enqueue it on the deferred ElementDeleter
// (GC-safe teardown at the next net_pump Flush). No-op if `eid` is unbound or its
// type is not a streamed mirror kind (Player/Kerfur/Unknown are not retired here).
void RetireMirror(coop::element::ElementId eid);

}  // namespace coop::element
