// coop/piramid_sync.h -- the walking-pyramid (piramid2_C) event mirror lane.
//
// Ground truth: research/findings/votv-piramid2-RE-2026-07-04.md; design: docs/events/piramid.md.
// The pyramid rides the GENERIC rails for four of its five axes -- spawn/pose/despawn =
// world_actor_sync (piramid2_C on kWorldActorAllowlist), the 4 killerwisps + their deaths =
// npc lane, event identity = event_fire_sync verdict ('piramid' no-replay), registry parity =
// the mirror's own native BeginPlay/Destroyed setEvent(true/false). This lane owns ONLY the
// one axis no generic rail carries: the pyramid's BRAIN (host-random AI) and the GATHER
// choreography relay. It is the second event-specific choreography lane (killerwisp-vs-peers
// was the first); a shared "brainy event actor" helper gets extracted when a third lane
// proves the common shape ([[feedback-fix-then-generalize-mirror-identity]]).
//
//  - CLIENT brain suppression: PRE-cancel the mirror's three STATE-WRITING timer handlers
//    (seeWisps / checkIfReached / randLoc -- every walkTo caller). ReceiveTick stays ALIVE
//    (deliberate divergence from the design doc's tick-off sketch, per RE section 5: the
//    per-tick gather-beam params, head look-at and hover-Z smoothing all live in the tick
//    and are pure derivations of mirrored state; march/turn are structurally zero because
//    isWalking/multiplyWalk can never latch once the walkTo callers are cancelled).
//    changeLook + the 30 s ping stay alive too -- per-viewer cosmetics by RE verdict.
//  - HOST gather detect: POST observer on checkIfReached (timer-fired -> ProcessEvent-
//    VISIBLE) edge-detects `gathering` false->true and relays PyramidGather{pyramidEid,
//    wispEid} (WorldActor eid + Npc eid -- the two lanes' own identities).
//  - CLIENT gather replay: stage the native inputs the host had at ITS commit (wispTarget +
//    isWalking, staged only once the mirrors interp within the native 10000-unit arrive
//    radius) and re-dispatch checkIfReached through a TLS allow slot -- the game's OWN
//    bytecode then runs the entire choreography (montage + proxy delegate binds + notifies +
//    beams + timelines + wisp freeze). Nothing is reimplemented. The 'del' notify's local
//    wisp destroy CONVERGES with the npc lane's authoritative EntityDestroy (a client mirror
//    is never in npc_sync's host reverse map -> no echo; whichever lands second sees
//    "already not-live" and no-ops).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PyramidGatherPayload;
}

namespace coop::piramid_sync {

// Store the session + latch install intent. Hook arming is LAZY (piramid2_C loads only when
// the event nears): Tick() arms the interceptors/observer once a piramid2_C WorldActor
// element exists (host: interceptor-allocated; client: wire-materialized).
void Install(coop::net::Session* session);

// Both roles, every gameplay tick (cheap internal gates):
//  pre-arm  -- 250 ms element-presence probe, then one-shot resolve+register (latched LOUD).
//  host     -- 1 s stale-entry sweep of the gather edge-detector map.
//  client   -- 250 ms actor-tick restore for new pyramid mirrors (world_actor parks generic
//              mirrors tick-off; the pyramid needs its tick for beams/look-at) + per-frame
//              pending-gather replay attempts while one is queued.
void Tick();

// HOST, game thread, at a joiner's world-ready edge (subsystems::ConnectReplayForSlot, AFTER
// world_actor_sync/npc_sync queued their snapshots so the mirrors will exist): if a gather is
// in flight RIGHT NOW (edge map holds gathering=true and the actor still points at a live
// wispTarget), re-send PyramidGather ToSlot -- the lane's late-join answer (COOP_EVENT_JOIN
// 3.4; without it a join DURING the ~10 s gather missed the whole choreography, because the
// commit relay is edge-triggered and fired before this peer connected).
void QueueConnectBroadcastForSlot(int slot);

// Clear per-session state (pending gather, edge map, restored-tick set, probe counters).
// Registered hooks stay latched for the process (role/session-gated inside, the
// npc/world_actor shape).
void OnDisconnect();

// Wire receiver (client): queue the gather for replay-when-converged. Called on the game
// thread (event_dispatch_entity posts it).
void OnPyramidGather(const coop::net::PyramidGatherPayload& payload);

// Probe/diagnostic accessors (autotest_piramidforce; thread-safe atomics).
bool DebugHooksArmed();
int  DebugHostRelayCount();
int  DebugClientReplayCount();

}  // namespace coop::piramid_sync
