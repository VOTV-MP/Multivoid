// coop/creatures/roach_sync.h -- host-authoritative roach-infestation mirror
// (v108, 2026-07-10).
//
// GROUND TRUTH (bytecode RE, research/bp_reflection/cockroachMaster.json +
// ticker_roachSummoner.json, 2026-07-10):
//   - Roaches are NOT actors: they are UStaticMeshComponent entries in the ONE
//     world-anchored AcockroachMaster_C's `roaches` TArray (gamemode.
//     cockroachMaster @0x910; roaches @0x238; maxAmount=128 CDO).
//   - The sim is SHARED-WORLD state, not a player effect: nests spawn near
//     FOOD props (spawnNest), roaches multiply near existing roaches
//     (addRoach at a member's location), and calc() -- driven from the
//     master's ReceiveTick -- moves each roach AND mutates shared props
//     (drains prop_food_C.foodData while eating, K2_DestroyActor on depleted
//     food, grows the roach's scale). Any peer can stomp (steppedOn/tryCrush
//     -> prop_deadRoach_C husk + deleteRoach) or EAT a roach.
//   - A client running its own sim therefore DIVERGES the shared food/roach
//     state (the docs/COOP_WORLD_PROP_DIVERGENCE root, component flavor).
//
// DESIGN (mirror STATE + drive the notify-free re-appliers, the serverbox
// shape -- [[lesson-votv-world-system-sync-mirror-state-not-verb]]):
//   - CLIENT suppression lives in coop/world/spawn_authority: t1 park of
//     cockroachMaster_C + ticker_roachSummoner_C actor tick, t3 PRE-cancels
//     of summonRoach + the 3 looping timer delegates (addRoachTimer /
//     spawnNestTimer / CustomEvent -- timers fire independently of the park).
//     Interaction EVENTS (eat/stomp) are NOT tick-driven and stay native.
//   - HOST polls the roaches array at 1 Hz and broadcasts a PAGED full
//     snapshot (RoachState: seq + page + per-roach {loc, scale}) on
//     population change, on drift, and as a periodic keepalive.
//   - CLIENT applies by ORDINAL (k-th valid slot; both sides' arrays keep
//     null holes, so ordinals -- not raw indices -- are the stable mapping):
//     count equal -> drive positions/scales (K2_SetWorldLocation /
//     SetWorldScale3D on the component); count differs -> REBUILD (reflected
//     deleteRoach(idx, crush=false) for every local roach, then
//     addRoach(loc, size, bypassCheck=true) per snapshot entry -- both are
//     the game's own BP functions, so the array/Count/collision stay
//     consistent with SP).
//   - CLIENT local consumption (a player ate/stomped a roach: the native
//     event ran locally, destroying the component): detected by liveness on
//     the tracked component set (no polling dispatches) -> RoachConsumed
//     {last known loc} intent to the host, which deletes its nearest roach
//     within the adjudication radius. The next snapshot converges everyone.
//   - The dead-roach husk (prop_deadRoach_C) the acting peer spawns locally
//     is NOT mirrored in Inc-1: its BeginDeferred runs inside the script fn
//     tryCrush (the chipPile-family EX shape -- PE-invisible), and it is a
//     300 s cosmetic. Known gap, not a correctness hole.
//
// Principle 7: this is coop/ network logic; all engine access via ue_wrap
// reflection/call. One concept, one folder: roach infestation = a creature
// system -> coop/creatures/.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct RoachStatePayload;
struct RoachConsumedPayload;
}  // namespace coop::net

namespace coop::roach_sync {

// Cache the session pointer (boot-lifetime; the session_holder pattern).
void Install(coop::net::Session* session);

// 1 Hz driver (TickGameplay, game thread). HOST: poll the roaches array ->
// broadcast paged snapshots on change/drift/keepalive. CLIENT: liveness-scan
// the tracked mirror set -> RoachConsumed intents for locally-eaten roaches.
void Tick();

// CLIENT receive: assemble pages of one snapshot seq; apply when complete
// (host-sender-checked). Game thread (event_feed drain).
void OnState(const coop::net::RoachStatePayload& payload, int senderPeerSlot);

// HOST receive: a client consumed a roach locally (eat/stomp) -- delete the
// nearest live roach within the adjudication radius. Game thread.
void OnConsumedIntent(const coop::net::RoachConsumedPayload& payload, int senderPeerSlot);

// HOST: unconditional current-population snapshot to a world-ready joiner
// (the serverbox connect-replay shape).
void QueueConnectBroadcastForSlot(int slot);

// Session teardown: clear assembly buffers, baselines, and the tracked set.
// (The t1 park restore lives in spawn_authority's own OnDisconnect.)
void OnDisconnect();

}  // namespace coop::roach_sync
