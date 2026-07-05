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
//  - CLIENT brain suppression: PRE-cancel the mirror's FOUR STATE-WRITING timer handlers
//    (seeWisps / checkIfReached / randLoc -- every walkTo caller -- plus changeLook, the
//    1 Hz RANDOM head-wander re-roll; v102 reclassified it from "per-viewer cosmetic" to a
//    streamed axis after the user saw the heads/searchlight diverge live 2026-07-05).
//    ReceiveTick stays ALIVE (deliberate divergence from the design doc's tick-off sketch,
//    per RE section 5: the per-tick gather-beam params, head look-at and hover-Z smoothing
//    all live in the tick and are pure derivations of mirrored state; march/turn are
//    structurally zero because isWalking/multiplyWalk can never latch once the walkTo
//    callers are cancelled). The 30 s ping stays alive -- per-viewer cosmetic by RE verdict.
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

// HOST pose augmentation (v100 auxYaw, called by world_actor_sync::TickPoseStream per pyramid
// entry, game thread): read the actor's VISIBLE heading -- the movementVector ArrowComponent's
// world yaw (the actor root never yaws; the AnimBP orients the body off the component). Returns
// false pre-arm / on an offset miss (caller falls back to the actor yaw).
bool ReadHostHeadingYaw(void* actor, float& outYaw);

// CLIENT facing drive (the auxYaw consumer): write `yaw` to the pyramid mirror's BOTH heading
// ArrowComponents -- the exact state the host's Turning step maintains. Called by
// world_actor_sync's client drive after each pose apply for a piramid2_C mirror.
void ApplyMirrorHeadingYaw(void* actor, float yaw);

// HOST pose augmentation (v102 auxVec): read the actor's `relLook` -- the head/searchlight's
// idle look TARGET (relative frame), natively re-rolled at 1 Hz by the RANDOM changeLook
// timer. Returns false pre-arm / on an offset miss (caller streams zeros; client keeps its
// last target).
bool ReadHostRelLook(void* actor, float& outX, float& outY, float& outZ);

// CLIENT head drive (the auxVec consumer): write the streamed relLook onto the pyramid
// mirror; its ALIVE native tick then eases the lookat component toward it (VInterpTo speed
// 1.0 -- the same playout the host runs). The mirror's own changeLook re-roll is PRE-
// cancelled, so this is the only writer. relLook is a plain BP var (the native writer is a
// simple assignment -- no setter side effects), so a raw write is the faithful mirror.
void ApplyMirrorRelLook(void* actor, float x, float y, float z);

// HOST pose augmentation (v104 auxTargetEid): the live wispTarget's npc-lane eid (0 = none /
// pre-arm). Cached per target change -- callable at the 60 Hz pose stream. The 0y-residue
// root: during the walk-to-wisp phase the host head runs the tick's CHASE branch (world
// location of wispTarget) while relLook keeps re-rolling ignored; the mirror needs the
// IDENTITY, not the idle vector, to run the same branch.
uint32_t ReadHostWispTargetEid(void* actor);

// CLIENT target drive (the auxTargetEid consumer): resolve the eid via the npc mirror table
// and set/clear the mirror's wispTarget so its ALIVE native tick picks the same look-at
// branch as the host (chase converges on the mirrored wisp; 0 falls back to relLook idle).
// Never touches the field while the mirror is `gathering` (the gather choreography owns it).
void ApplyMirrorWispTarget(void* actor, uint32_t wispEid);

// Probe/diagnostic accessors (autotest_piramidforce; thread-safe atomics).
bool DebugHooksArmed();
int  DebugHostRelayCount();
int  DebugClientReplayCount();

}  // namespace coop::piramid_sync
