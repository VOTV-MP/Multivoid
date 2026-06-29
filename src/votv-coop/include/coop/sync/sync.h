// coop/sync/sync.h -- the consolidated world-sync module facade.
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
// ASSEMBLY STATE (2026-06-28, ASSEMBLED). The identity module -- one eid<->actor
// owner across create / adopt / morph / destroy -- is built:
//   - CreateOrAdopt ....... sync_create.h: the ONE bind decision (idempotent-adopt
//                           / morph-reskin / Install-new w/ HEAD live-conflict
//                           reject). Every prop-mirror bind funnels here
//                           (RegisterPropMirror forwards; OnSpawn + both convert
//                           morph sites route through it).
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
// net_pump) + the purge-episode flag into a coop::sync::Tick. Inspection refutes it:
// the two caches are CONSCIOUSLY separate -- the seeder's piggybacks the seed walk's
// existing GUObjectArray iteration (perf audit W-2 forbids a per-walk FindObjectsBy-
// Class; the 120->60fps lesson), the reaper's is a throttled FindObjectByClass for
// its gameplay-vs-menu name check (documented net_pump.cpp:425, engine.cpp:60). The
// purge flag is already a clean single-writer pub/sub (net_pump detects -> tracker
// owns the atomic -> gate/sweep/reconcile read). Folding would regress perf or couple
// subsystems for zero functional gain = a crutch (RULE 1) + churn (RULE 2). Left as-is.

#pragma once

#include "coop/sync/sync_reconcile.h"   // RunIdentityReconcile / OnReconcileTick (the non-one-shot reconcile)
#include "coop/sync/sync_create.h"      // CreateOrAdopt (the one collision-reconcile create/bind path)
#include "coop/sync/sync_destroy.h"     // RetireMirror (the one type-dispatched mirror destroy funnel)
