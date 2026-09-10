// coop/creatures/piramid_sync.h -- the walking-pyramid (piramid2_C) event mirror lane.
//
// The pyramid rides the GENERIC rails for four of its five axes -- spawn/pose/despawn =
// world_actor_sync (piramid2_C on kWorldActorAllowlist), the 4 killerwisps + their deaths =
// npc lane, event identity = event_fire_sync verdict ('piramid' no-replay), registry parity =
// the mirror's own native BeginPlay/Destroyed setEvent(true/false). This lane owns ONLY the
// one axis no generic rail carries: the pyramid's BRAIN (host-random AI) and the GATHER
// choreography relay. It is the second event-specific choreography lane (killerwisp-vs-peers
// was the first); a shared "brainy event actor" helper gets extracted when a third lane
// proves the common shape. Player-facing behaviour: docs/events-and-weather.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PyramidGatherPayload;
}

namespace coop::piramid_sync {

// CLIENT brain suppression, armed lazily by Tick below: PRE-cancel the FOUR STATE-WRITING timer
// handlers -- seeWisps / checkIfReached / randLoc (every walkTo caller), plus changeLook, the
// 1 Hz RANDOM head-wander re-roll, which is a STREAMED axis because left free the heads and
// searchlight diverge visibly between peers. ReceiveTick stays ALIVE, deliberately: the
// per-tick gather-beam params, head look-at and hover-Z smoothing are pure derivations of
// mirrored state, and march/turn are structurally zero once the walkTo callers are cancelled,
// because isWalking/multiplyWalk can never latch. The 30 s ping stays alive, per-viewer only.
//
// Store the session + latch install intent. Hook arming is LAZY (piramid2_C loads only when
// the event nears): Tick() arms the interceptors/observer once a piramid2_C WorldActor
// element exists (host: interceptor-allocated; client: wire-materialized).
void Install(coop::net::Session* session);

// Tick also arms and maintains the HOST gather detect. The work is the observer's: a POST
// observer on checkIfReached (timer-fired, so ProcessEvent-VISIBLE) edge-detects `gathering`
// false->true and relays PyramidGather{pyramidEid, wispEid} -- the WorldActor eid and the Npc
// eid, each lane's own identity. Tick's own host job is the 1 Hz stale-edge sweep.
//
// Both roles, every gameplay tick (cheap internal gates):
//  pre-arm  -- 250 ms element-presence probe, then one-shot resolve+register (latched LOUD).
//  host     -- 1 s stale-entry sweep of the gather edge-detector map.
//  client   -- 250 ms actor-tick restore for new pyramid mirrors (world_actor parks generic
//              mirrors tick-off; the pyramid needs its tick for beams/look-at) + per-frame
//              pending-gather replay attempts while one is queued.
void Tick();

// HOST, game thread, at a joiner's world-ready edge (subsystems::ConnectReplayForSlot,
// AFTER world_actor_sync and npc_sync queued their snapshots so the mirrors will exist):
// if a gather is in flight RIGHT NOW -- read off the live actors, whose `gathering` flag
// is latched and whose wispTarget still resolves, never off the edge map, which lags the
// 1 Hz sweep -- re-send PyramidGather ToSlot. That is the lane's late-join answer
// (docs/events-and-weather.md): without it a join DURING the roughly 10 s gather misses
// the whole choreography, because the commit relay is edge-triggered and fired before
// this peer connected.
void QueueConnectBroadcastForSlot(int slot);

// Clear per-session state (pending gather, edge map, restored-tick set, probe counters).
// Registered hooks stay latched for the process (role/session-gated inside, the
// npc/world_actor shape).
void OnDisconnect();

// Wire receiver (client): queue the gather for replay-when-converged. Called on the game
// thread (event_dispatch_entity posts it).
//
// The replay stages the native inputs the host held at ITS commit (wispTarget + isWalking,
// staged once the mirrors interp inside a 10500-unit pre-gate, deliberately WIDER than the
// native 10000-unit arrive radius, the re-dispatched native check being the real arbiter) and
// re-dispatches checkIfReached through a TLS allow slot, so the game's OWN bytecode runs the
// whole choreography -- montage, proxy delegate binds, notifies, beams, timelines, wisp
// freeze. Nothing is reimplemented. The 'del' notify's local wisp destroy CONVERGES with the
// npc lane's authoritative EntityDestroy: a client mirror is never in npc_sync's host reverse
// map, so there is no echo, and whichever lands second sees "already not-live" and no-ops.
void OnPyramidGather(const coop::net::PyramidGatherPayload& payload);

// HOST pose augmentation (auxYaw, called by world_actor_sync::TickPoseStream per pyramid
// entry, game thread): read the actor's VISIBLE heading -- the movementVector
// ArrowComponent's world yaw, since the actor root never yaws and the AnimBP orients the
// body off the component. Returns false pre-arm or on an offset miss, and the caller falls
// back to the actor yaw.
bool ReadHostHeadingYaw(void* actor, float& outYaw);

// CLIENT facing drive (the auxYaw consumer): write `yaw` to the pyramid mirror's BOTH heading
// ArrowComponents -- the exact state the host's Turning step maintains. Called by
// world_actor_sync's client drive after each pose apply for a piramid2_C mirror.
void ApplyMirrorHeadingYaw(void* actor, float yaw);

// HOST pose augmentation (auxVec): read the actor's `relLook` -- the head and searchlight's
// idle look TARGET in the relative frame, natively re-rolled at 1 Hz by the random
// changeLook timer. Returns false pre-arm or on an offset miss; the caller then streams
// zeros and the client keeps its last target.
bool ReadHostRelLook(void* actor, float& outX, float& outY, float& outZ);

// CLIENT head drive (the auxVec consumer): write the streamed relLook onto the pyramid
// mirror; its ALIVE native tick then eases the lookat component toward it (VInterpTo speed
// 1.0 -- the same playout the host runs). The mirror's own changeLook re-roll is PRE-
// cancelled, so this is the only writer. relLook is a plain BP var (the native writer is a
// simple assignment -- no setter side effects), so a raw write is the faithful mirror.
void ApplyMirrorRelLook(void* actor, float x, float y, float z);

// HOST pose augmentation (auxTargetEid): the live wispTarget's npc-lane eid, 0 for none or
// pre-arm. Cached per target change, so it is callable at the 60 Hz pose stream. It exists
// because during the walk-to-wisp phase the host head runs the tick's CHASE branch, on the
// world location of wispTarget, while relLook keeps re-rolling ignored: the mirror needs the
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

}  // namespace coop::piramid_sync
