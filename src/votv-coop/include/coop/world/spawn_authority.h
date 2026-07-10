#pragma once

// coop/world/spawn_authority.h -- the ONE owner of client-side shared-world
// spawner suppression (T1 structural fix, Inc-1).
//
// Ratified design: docs/COOP_RNG_AUTHORITY.md "T1 STRUCTURAL DESIGN" (2026-07-10,
// /qf DESIGN pass, 6 rounds). INVARIANT: a connected CLIENT ticks NO shared-world
// spawner and rolls NO shared-world spawn RNG; shared-world content arrives ONLY
// via the host wire (the npc_sync / world_actor / prop mirrors). Fork evidence:
// deer/bp7/hexahive spawners measured ROLLING on a connected client (THIRD CENSUS).
//
// Mechanism = ONE table, tiers by class shape:
//   t1 PARK   -- instance tick-park via the engine's own SetActorTickEnabled
//                (join-window pass + 1 Hz re-park of the cached set + 15 s
//                reconcile walk for late instances), RESTORED on session end
//                (suppression is a loan; the mandatory menu teardown is the
//                structural repayment, the restore is belt).
//   t3 CANCEL -- PRE-cancel interceptor on the spawner's entry fn (the proven
//                ambient shape; ProcessEvent-dispatched fns only).
//   (t2 shared driver-native caller filter -- Delay/SetTimerDelegate starve --
//    ships with the first Delay-latent Inc-2 family (beehive/mannequin); no
//    Inc-1 class needs it, so it does not exist yet by design.)
//
// ACTIVE ROWS (dump-verified; REVISED 2026-07-10 eve -- the anchor audit):
//   t1 ticker_insomniacSpawner_C   -> insomniac_C(+_Child_C)  [product in kNpcAllowlist]
//   t1 ticker_fossilhoundSpawner_C -> fossilhound_C           [product in kNpcAllowlist]
//   t1 cockroachMaster_C + ticker_roachSummoner_C  [roach lane v108: the master's
//      tick runs calc() = movement + SHARED-prop mutation (food drain/destroy);
//      coop/creatures/roach_sync drives the client population from host state]
//   t3 mushroomMaster_C::Spawn / mushroomSpawner_C::Spawn /
//      ticker_yellowWispSpawner_C::ReceiveTick  [mushrooms spawn at their own
//      world transform; yellow wisp anchors on a navmesh RANDOM-WALK point --
//      both world-anchored, killerwisp_C host-mirrored]
//   t3 ticker_wispSpawner_C::ReceiveTick  [sky wisps at ABSOLUTE map coords
//      (+-60-70k X/Y, Z=80k) -- REVERSED from OWNER-EFFECT 2026-07-10; products
//      wisp_C + 8 color variants host-mirrored via the EX-catch source row]
//   t3 cockroachMaster_C::{summonRoach,addRoachTimer,spawnNestTimer,CustomEvent}
//      [the 3 looping timer delegates fire independently of the t1 park]
//
// REMOVED ROW (2026-07-10 eve): pineconeSpawner_C::ReceiveTick -- measured
//   anchor is GetPlayerCameraManager->GetActorLocation (drops land around the
//   LOCAL player) -> OWNER-EFFECT: each peer rolls its own; the peer-symmetric
//   ambient broadcaster in coop/props/host_spawn_watcher cross-mirrors them.
//
// CLASSIFIED, NOT ROWS (the full 28-ticker gate read, 2026-07-10):
//   OWNER-EFFECT (user rule [[feedback-owner-effect-rule]] -- per-peer AUTHORITY
//     + cross-peer mirror; NEVER parked): ticker_fireflySpawner_C (measured:
//     player-camera anchor + grass trace -> emitter; mirrored by firefly_sync
//     v51), pineconeSpawner_C (measured: player-camera anchor; owner-mirrored
//     via host_spawn_watcher 2026-07-10). ticker_wispSpawner_C was MISCLASSIFIED
//     here (assumed player-anchored; the dump shows absolute map coords) -- now
//     a t3 row above. "Autumn leaves" remain UNVERIFIED (no leaf ticker in the
//     28 dumps; RE before classifying).
//   Inc-2 FAMILIES (product NOT mirrored yet -- mirror FIRST, then park):
//     deerSpawner->deer_C, bp7Spawner->NewBlueprint7_C, hexahiveSpawner,
//     eyers->eyer_C, treeSpawner->walkingTree_C, susHoleSpawner->susDirtHole_C,
//     beehiveSpawner->beehiveBranch_C (Delay-latent -> needs t2),
//     bushSpawning (data-table flora), ticker_tick->NewBlueprint17_C,
//     mannequinSpawner (MIXED: K2_DestroyActor reap duty -> needs
//     (class,latent-UUID) grain per the design). roachSummoner GRADUATED to
//     active rows (roach lane v108).
//   NON-SPAWNERS (never rows): serverBreaker (serverbox_sync owns it),
//     dishUncalib/disher (T2 lanes), flickerer (T3 cosmetic), foodHall/
//     foodSleeper/foodTolerance (player-local vitals; foodSleeper is the idle
//     starvation-damage source), surveilanceMaster, widgetRender, paused,
//     base, kerf.
//   PER-PEER BY DOCTRINE: undergroundGarbageSpawner_C -> dirthole_item_C (the
//     per-peer loot mounds; explains the census RESIDUE row -- suppressing it
//     would delete client loot with no host replacement).
//
// The BeginDeferred interceptor (npc_sync) stays the wire-bypass + MIRROR seam;
// its client pass-through branch calls NoteClientSpawnPassThrough as the
// shipping TRIPWIRE (a table spawner class spawning on a connected client =
// regression alarm + the late-instance signal). New-class DISCOVERY stays with
// the ini-gated rng_roll_census (dev tooling), not this module.

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::spawn_authority {

// Register the t3 PRE-cancel interceptors + resolve the t1 park classes (lazy
// ~1 Hz retry until all resolve, then latched). Stores `session` for the
// per-dispatch / per-tick gates. Game thread (net_pump InstallObservers ensure).
void Install(coop::net::Session* session);

// t1 park driver (TickGameplay, game thread): on an ACTIVE CLIENT session runs
// the initial park pass, the 1 Hz cached re-park, and the 15 s late-instance
// reconcile walk; on session end (any path) restores parked ticks. Cheap
// early-return when not an active client session and nothing is parked.
void Tick();

// Session-end restore (subsystems::DisconnectAll fanout). Re-enables tick on
// every still-live parked instance + clears the cache. Tick()'s gate also
// restores if the fanout is missed (belt per the suppression-loan lesson).
void OnDisconnect();

// TRIPWIRE + late-instance signal, called from npc_sync's client pass-through
// branch (fires on parallel-anim worker threads too -- this only compares
// pre-resolved class pointers + throttles a WARN; no walks, no engine calls).
// Returns true iff actorClass is a t1 park class (caller lets the spawn
// proceed; the reconcile walk parks the new instance within ~15 s).
bool NoteClientSpawnPassThrough(void* actorClass);

}  // namespace coop::spawn_authority
