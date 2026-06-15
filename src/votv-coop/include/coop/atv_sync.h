// coop/atv_sync.h -- ATV/quadbike (AATV_C) Phase 1 body POSE sync (protocol v47).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the occupant-authority gating,
// the per-tick stream/poll, the receiver kinematic apply with a LerpWindow interp, the key->actor
// index, and the connect-snapshot. Talks to the engine ONLY through ue_wrap::atv.
//
// MODEL (RULE 1 / follow-MTA CUnoccupiedVehicleSync). The ATV is a save-placed, KEYED entity
// (Key@0x0618, cross-peer stable -- like the appliances/grime, NOT a runtime-spawned NPC), so it
// is synced as a keyed pose stream, NOT through the eid/Element/spawn machinery (that is YAGNI for
// one always-present keyed actor -- the same divergence the grime/window dirt sync made vs its
// element blueprint). OCCUPANT-AUTHORITATIVE single-syncer: the peer whose LOCAL player is seated
// (the driver) reads its live ATV root transform and throttled-streams it (~20Hz) reliably; the
// host RELAYS a client driver's pose to the other clients. A receiver applies it KINEMATICALLY
// (ue_wrap::atv::PrepareMirror disables the mirror's physics+tick so its own rig can't fight the
// stream) interpolated by a coop::LerpWindow, UNLESS it is itself the occupant (then it ignores --
// it is the authority, and re-enables its physics via ReleaseMirror so it can drive). Unoccupied
// ATVs are not streamed (they hold last pose); the host connect-snapshots each ATV's current pose
// (adopt=1) to a joiner.
//
// Phase 1 = body pose only. Phase 1.5 = occupant->puppet seating (occupantSlot is carried now).
// Phase 2 = fuel/health/brake on-change + the broken/explode discrete edge. Phase 1.5+ may move
// the transport to an UNRELIABLE newest-wins datagram (the blueprint's element path) if a lossy
// link makes the reliable ARQ lag the body -- on LAN/low-loss the reliable stream + interp is
// smooth. RE: research/findings/votv-ATV-quadbike-RE-and-coop-sync-design-2026-06-08.md +
// votv-ATV-Phase1-pose-stream-blueprint-2026-06-08.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AtvStatePayload;
struct AtvReleasePayload;
}  // namespace coop::net

namespace coop::atv_sync {

// Resolve AATV_C + build the key->actor index; store the session pointer. Idempotent; retried
// every net-pump tick until the class loads. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: an AtvState packet arrived (payload already memcpy'd + range-checked by
// event_feed). Resolves the ATV by Key and pushes the pose into its interp window (deferring if it
// has not streamed in yet), UNLESS this peer is the LOCAL AUTHORITY (occupant OR grav-hand grabber)
// of that ATV. Called from event_feed.
void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t senderPeerSlot);

// Receiver entry: an AtvRelease packet arrived (the grab-carry release/throw edge, v76). Resolves
// the ATV by Key, re-enables its mirror physics (ReleaseMirror) and THEN applies the inherited
// linear+angular velocity so it un-freezes and arcs/lands under its own simulation -- UNLESS this
// peer is the local authority (then it owns the physics and ignores it). Called from event_feed.
void OnAtvRelease(const coop::net::AtvReleasePayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current pose of every indexed ATV to a freshly connected client
// `peerSlot` (adopt=1 -> the joiner snaps to it). Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: throttled index rebuild; per ATV -> if this peer is the occupant, throttled-stream
// its pose (+ release the mirror if it had been one); else drive the mirror interp toward the last
// streamed pose. Call every net-pump tick on the game thread.
void Tick();

// Session teardown: re-enable physics on any ATV we were mirroring (don't leave it frozen in
// single-player), then clear the index + interp state.
void OnDisconnect();

}  // namespace coop::atv_sync
