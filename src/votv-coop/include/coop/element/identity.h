// coop/element/identity.h -- the entity-identity authority facade (coop::element).
//
// THE single conceptual sync engine (sync-consolidation refactor 2026-06-27,
// research/findings/sync-consolidation-refactor-PLAN-2026-06-27.md). One module,
// one identity owner, one create-or-adopt, one reconcile, one destroy funnel,
// one authority model -- behind this clean API. Callers (event dispatch, net
// pump, the engine hooks) talk to sync THROUGH this surface; the eid-bind /
// divergence-valve / purge-escalation / mirror-teardown complexity is private.
//
// THIS MODULE IS THE *ENTITY-IDENTITY* LAYER ONLY -- not "everything that sends a
// packet". Keyed-state replicators (doors/keypads/power/grime/atv...), global
// scalars (clock/sky/weather), and player-scoped/social (inventory/chat/voice)
// are SIBLINGS that route through the same dispatch but have no identity race, so
// they stay as their own small *_sync.cpp files. The full who-lives-where map is
// docs/COOP_SYNC_MAP.md (the discoverability index across all ~95 coop files).
//
// FOUNDATION DISCOVERY (plan section 1): the identity owner (`SyncRegistry`) is
// ALREADY played by `element::Registry` (the sole eid<->actor array, host/peer
// ranges, RegisterMirror/Get/FreeId/EidForActor) + `MirrorManager<T>` (the
// per-kind manager) + `Element` (the entity). So this module does NOT rebuild a
// registry -- it adds the missing seams on top.
//
// ASSEMBLY STATE (2026-06-29, AUTHORITY EXTENDED -- A/B/C). The identity module --
// one eid<->actor owner across create / adopt / morph / destroy -- is built, and
// the 2026-06-29 authority pass extended it from prop-only to ALL streamed kinds +
// added the compile wall:
//   - CreateOrAdopt ....... sync_create.h: the ONE bind decision (idempotent-adopt
//                           / morph-reskin / Install-new w/ HEAD live-conflict
//                           reject). Every PROP mirror bind funnels here
//                           (RegisterPropMirror forwards; OnSpawn + both convert
//                           morph sites route through it). [Inc A 2026-06-29] NOW
//                           ALSO NPC + WorldActor: CreateOrAdoptNpcMirror /
//                           CreateOrAdoptWorldActorMirror (one templated simple-
//                           mirror helper); npc_mirror/npc_adoption/world_actor_sync
//                           route their wire binds through sync, not raw Install.
//   - RetireMirror ........ [Inc B] sync_destroy.h: the ONE type-dispatched destroy
//                           funnel -- resolves the Element's ElementType, Takes from
//                           the matching MirrorManager<T>, Enqueues to ElementDeleter.
//                           The 6 per-producer Enqueue(Mgr.Take(eid)) sites
//                           (npc_sync x3, world_actor_sync x2, kerfur_reconcile)
//                           route through it.
//   - SEALED Install ...... [Inc C] MirrorManager<T>::Install is now PRIVATE,
//                           friended to exactly coop::element::MirrorInstallAccess. A
//                           wire mirror can ONLY be bound through sync -- a feature
//                           file reaching into a manager to Install is a COMPILE
//                           error ("someone to watch them", enforced not conventional).
//                           AllocAndInstall (host mint) + Take/Drop/Drain (teardown)
//                           stay public per-type-authority ops (sealing them = a
//                           god-module crutch, RULE 1 -- deliberately not done).
//   - SyncReconcile ....... sync_reconcile.h: the non-one-shot identity reconcile
//                           (valve-abort re-bind + post-purge window). VERIFIED
//                           16:06 (242 re-binds + 13 post-purge fires, world clean).
//   - Identity owner ...... element::Registry::EidForActor -- the unified
//                           actor->eid reverse for LOCALS AND mirrors. THREE leaked
//                           satellites deleted into it: g_propElementsById,
//                           g_boundMirrorNatives (-> Element::IsSaveNative),
//                           g_actorToPropElementId (-> EidForActor).
//   - Destroy funnel ...... element::ElementDeleter (Enqueue any-thread -> Flush GT
//                           at net_pump top-of-tick). 13 producers route Element
//                           teardown through it; bare-actor retires keep their
//                           site-specific pre-steps (proxy un-root / echo-suppress).
//   - Router .............. event_feed::Update default chains HandleEntity/State/
//                           World (each returns true iff it owns the kind). The
//                           family switch is the single membership declaration
//                           (the 3-place ReliableKind wiring hazard is now 2).
//
// AUTHORITY CONTRACT (the model already in force -- named here so it stops being
// implicit; this is what the D1/D2 race was a symptom of NOT having written down):
//   - HOST-AUTHORITATIVE (host decides, broadcasts, peers apply): NPC lifecycle
//     (EntitySpawn/Destroy), WorldActor lifecycle, kerfur form-convert
//     (kerfur_convert: client sends a *request*, host authors + broadcasts
//     KerfurConvert), world/ambient state (time/sky/weather), combat (PlayerDamage,
//     WispGrab/Tear), balance.
//   - CLIENT-RELAY-INTENT (client sends intent, host authors the result): chipPile
//     grab/throw (GrabIntent/ThrowIntent -> host runs the verb -> PropConvert/Pose
//     broadcast). The host is the single writer; the client never self-applies.
//   - PEER-SYMMETRIC (any peer originates, others mirror): held-prop PropPose stream,
//     ambient firefly, chat.
//   DEFERRED (the one open behavior change, NOT built -- gated on the post-assembly
//   hands-on per the verify-after-assembly rule): a host->client corrective-pose
//   for an *adopted* kerfur off-prop. Today the client adopts by eid (works) but
//   keeps driving the adopted prop on its local held-pose stream with no host
//   correction -> the D2 twitch / hang-in-air symptoms. The fix lands as a granted-
//   syncer revoke + a one-shot authoritative pose on convert; see the plan's
//   SyncAuthority row + the 3 OPEN SYMPTOMS. Do NOT scaffold it speculatively
//   (RULE 2) -- it is built WITH its hands-on, not before.
//
// RESIDUE -- folding REJECTED on inspection (2026-06-28). The plan floated folding
// the two world-ptr caches (g_seedWorld in prop_element_tracker vs g_reapWorld in
// net_pump) + the purge-episode flag into a coop::element::Tick. Inspection refutes it:
// the two caches are CONSCIOUSLY separate -- the seeder's piggybacks the seed walk's
// existing GUObjectArray iteration (perf audit W-2 forbids a per-walk FindObjectsBy-
// Class; the 120->60fps lesson), the reaper's is a throttled FindObjectByClass for
// its gameplay-vs-menu name check (documented net_pump.cpp:425, engine.cpp:60). The
// purge flag is already a clean single-writer pub/sub (net_pump detects -> tracker
// owns the atomic -> gate/sweep/reconcile read). Folding would regress perf or couple
// subsystems for zero functional gain = a crutch (RULE 1) + churn (RULE 2). Left as-is.

#pragma once

#include "coop/element/quiescence_drain.h"     // the join-window order owner (RunReconcile / OnTick + the deferred queues)
#include "coop/element/identity_create.h"      // CreateOrAdopt (the one collision-reconcile create/bind path)
#include "coop/element/identity_destroy.h"     // RetireMirror (the one type-dispatched mirror destroy funnel)
