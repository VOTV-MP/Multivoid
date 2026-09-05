// coop/element/identity.h -- the entity-identity authority facade: one identity owner, one
// create-or-adopt, one reconcile, one destroy funnel, one authority model. Callers (the event
// dispatch, the net pump, the engine hooks) reach the identity layer through it; the eid
// bind, the divergence valve, the purge escalation and the mirror teardown are private. The
// entity-identity layer only: keyed-state replicators, global scalars and player-scoped
// lanes are siblings with no identity race and stay their own files. The owner is the
// element registry (the sole eid-to-actor array, the host and peer ranges, the actor-to-eid
// reverse for locals and mirrors) with the per-kind mirror managers; this facade adds the
// seams: the one bind decision (identity_create.h: adopt, morph re-skin, or install with a
// live-conflict reject), the one type-dispatched destroy funnel (identity_destroy.h), the
// join-window order owner (quiescence_drain.h), and a sealed manager install, so a wire
// mirror binds only through the facade and a feature file reaching into a manager fails to
// compile. The authority contract: host-authoritative (the NPC and world-actor lifecycles,
// the kerfur conversion, world state, combat, balance); client-relay-intent (the chip-pile
// grab and throw: the client sends intent, the host authors); peer-symmetric (poses, chat).

#pragma once

#include "coop/element/quiescence_drain.h"     // the join-window order owner (RunReconcile / OnTick + the deferred queues)
#include "coop/element/identity_create.h"      // CreateOrAdopt (the one collision-reconcile create/bind path)
#include "coop/element/identity_destroy.h"     // RetireMirror (the one type-dispatched mirror destroy funnel)
