// coop/drone_sync.h -- the delivery DRONE (Adrone_C), body pose, on ReliableKind::DroneState.
// Gameplay/network layer (principle 7): the wire protocol, the host-authoritative transform stream,
// the receiver's kinematic apply through a LerpWindow, and the connect snapshot, reaching the
// engine only through ue_wrap::drone.
//
// The drone is a host-simulated singleton, MTA's server-simulated entity: its blueprint ReceiveTick
// is a fragile per-tick float flight integrator, not worth reproducing bit-exact on a remote. The
// host streams the resolved transform at about 20 Hz while the drone is active; a client suppresses
// its own drone tick, so the drone there is ALWAYS a mirror rather than a second flier fighting the
// stream, and drives the streamed transform kinematically through the interpolation window. Host to
// client only, never relayed, honoured only from slot 0. Identity is the singleton found by class:
// both peers load the same placed drone, and a joiner gets the current pose with adopt=1.
//
// The delivered CARGO (orderbox, giftbox, crate, dronesack -- all Aprop_C) rides the existing prop
// pipeline, so it needs no drone-specific packet.

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
