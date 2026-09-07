// coop/dev/atv_probe.h -- the ATV baseline instrument.
//
// AATV_C is a CONSTRAINT RIG whose whole visible output is suspension travel, so the lane that
// mirrors it does not freeze a mirror; it lets the rig simulate and corrects it. A corrector cannot
// be judged without a baseline, and this is what measures one. It writes one line per ATV per
// sample, prefixed [ATVP], designed to be diffed host against client. It is not a selftest and
// asserts nothing.
//
// Costs nothing when off, and never walks GUObjectArray: it registers a coop::element::scan_hub
// consumer and is HANDED its ATVs by the one shared pass. Enable with [dev] atv_probe=1
// (multivoid.ini) on both peers.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::atv_probe {

// Read the ini once, register the scan-hub consumer. Idempotent; cheap when off. Game thread.
//
// What a sample holds, per ATV, on BOTH peers:
//   - vehicleGetParts(), the game's OWN matched read of all four rig bodies (body, frontLeft,
//     frontRight, backWheelRoot), world location and rotation each.
//   - the rig's INTERNAL geometry: each wheel's offset from the body, in BODY SPACE. This is the
//     measurement that matters, because a rigid frozen rig holds those three vectors CONSTANT and a
//     live suspension makes them breathe, so "is the mirror a frozen corpse" becomes a number
//     rather than an adjective.
//   - vitals -- fuel, battery, dirt, health -- which measure whether an idle ATV self-simulates and
//     diverges because every peer ticks it.
//   - isDriven and the occupant, so a sample can be attributed to a driving peer.
void Install();

// Sample on cadence and log; drive the arm when armed. Call once per net-pump tick (game thread).
// Takes the session because the arm must not fire before a peer is WORLD-READY: the whole point is
// to measure a MIRROR, and a drive window that opens while the other peer is still loading the save
// measures an ATV nobody is mirroring.
//
// THE DRIVE ARM ([dev] atv_probe_sit=1). Sampling an IDLE ATV measures nothing about a mirror,
// because an idle ATV is never mirrored -- atv_sync releases an unauthored one and no peer streams
// it -- and an authored but STATIONARY one is not enough either, since the two peers agree to
// 0.3 cm at rest. Only a moving rig puts the corrector under load, so the arm seats a player,
// releases the handbrake and drives. It is the ONLY write the probe makes, it is off by default,
// and it belongs on exactly ONE peer: two armed peers race for the same rig. Set it on the client
// rather than the host, since the seat verb refuses a player whose hands are full. The seat verb,
// its three gates and the five terms that decide whether the throttle produces torque are in
// coop/dev/atv_probe.cpp.
void Tick(coop::net::Session& session, bool isHost);

}  // namespace coop::dev::atv_probe
