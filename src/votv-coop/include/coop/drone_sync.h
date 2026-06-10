// coop/drone_sync.h -- delivery DRONE (Adrone_C) Phase 1 body pose sync (protocol v48).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the host-authoritative transform
// stream, the receiver kinematic apply with a LerpWindow interp, and the connect-snapshot. Talks
// to the engine ONLY through ue_wrap::drone.
//
// MODEL (RULE 1 / follow-MTA server-simulated-entity). The delivery drone is a SINGLETON,
// host-simulated actor (its BP ReceiveTick is a fragile per-tick float flight integrator -- not
// worth reproducing bit-exact on a remote). So: HOST-AUTHORITATIVE -- the host streams the resolved
// actor transform (~20Hz while Active); the CLIENT suppresses its own drone ReceiveTick (so it
// can't fly on its own + fight the stream -- the drone is ALWAYS a mirror on the client) and drives
// the streamed transform kinematically, interpolated by a coop::LerpWindow. HOST->client only (not
// relayed; trust-gated to senderPeerSlot==0, like SkyState/TimeSync). Identity = SINGLETON
// (FindObjectByClass(drone_C) -- no key; both peers load the same placed drone). The host connect-
// snapshots the current pose (adopt=1) to a joiner.
//
// Phase 1 = body pose only. The delivered CARGO (orderbox/giftbox/crate/dronesack -- all Aprop_C)
// rides the EXISTING prop pipeline (PropSpawn/PropPose), so it needs no drone-specific packet.
// Phase 2 = flyingType/hasSack FX gating + the console-call client->host request + the radar Active
// edge + the economy order/sell edges. RE: research/findings/votv-delivery-drone-RE-and-coop-sync-
// design-2026-06-03.md.

#pragma once

namespace coop::net {
class Session;
struct DroneStatePayload;
}  // namespace coop::net

namespace coop::drone_sync {

// Resolve Adrone_C + store the session pointer. Idempotent; retried every net-pump tick until the
// class loads. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a DroneState packet arrived (payload memcpy'd + range-checked by event_feed,
// trust-gated to the host). The CLIENT suppresses its drone tick + pushes the pose into the interp
// window. The host defensively no-ops. Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::DroneStatePayload& payload);

// HOST-only: snapshot the drone's current pose (adopt=1) to a freshly connected client `peerSlot`
// so the joiner snaps to it (mid-flight or parked). Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: HOST streams the drone transform while Active (+ one falling-edge inactive);
// CLIENT suppresses the drone tick once + drives the mirror interp. Call every net-pump tick on the
// game thread.
void Tick();

// Session teardown: restore the drone's tick (so single-player flight works again) + reset state.
void OnDisconnect();

}  // namespace coop::drone_sync
