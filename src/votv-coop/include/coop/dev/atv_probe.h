// coop/dev/atv_probe.h -- the ATV baseline instrument (C1, 2026-08-29).
//
// WHAT QUESTION THIS ANSWERS
// --------------------------
// The C1 redesign (research/findings/vehicles/votv-ATV-full-sync-DESIGN-2026-08-29.md)
// rests on pillar P1 -- mirrors SIMULATE and are CORRECTED, because AATV_C is a
// CONSTRAINT RIG whose entire visible output is suspension travel. Every claim about
// what the SHIPPED lane does to that rig is [RD] from bytecode, never [V] from a
// running game: docs/vehicles/ATV.md 11.1 is still the open question
// "do mirrored wheels follow the body?"
//
// A corrector cannot be judged without a baseline, so this measures the baseline.
// It samples, on BOTH peers, per ATV:
//
//   - vehicleGetParts() -- the game's OWN matched read of all four rig bodies
//     (body / frontLeft / frontRight / backWheelRoot), world loc + rot each.
//   - the rig's INTERNAL geometry: each wheel's offset from the body, expressed in
//     BODY SPACE. This is the measurement that matters. A rigid, frozen rig holds
//     those three vectors CONSTANT; a live suspension makes them breathe. So
//     "is the mirror a frozen corpse" becomes a number, not an adjective.
//   - vitals: fuel / battery / dirt / health, to measure the P4 claim that an idle
//     ATV self-simulates and DIVERGES because every peer ticks it.
//   - isDriven + occupant, so a sample can be attributed to a driving peer.
//
// It writes one line per ATV per sample, prefixed [ATVP], designed to be diffed
// host-vs-client. It is not a selftest and asserts nothing: at this stage we do not
// know the right answer, which is the point.
//
// THE DRIVE ARM ([dev] atv_probe_sit=1, HOST only, one shot).
// Sampling an IDLE ATV measures nothing about a MIRROR, because an idle ATV is never
// mirrored: atv_sync.cpp releases an unauthored one and no peer streams it. So the open
// question -- docs/vehicles/ATV.md 11.1, do a mirror's wheels follow its body -- needs
// the ATV AUTHORED, which needs an occupant. And an authored but STATIONARY ATV is not
// enough either: the 2026-08-29 baseline (ATV.md 13) measured the two peers already
// agreeing to 0.3 cm at rest, so only a MOVING rig puts the corrector under load.
//
// So the arm seats and then drives, in three measured steps (all disasm-cited in the
// .cpp): actionName(player, hit, "sit") -> release the handbrake via the game's own
// setBrake() -> hold input_forward for 30 s -> dismount(). It is the ONLY write the probe
// makes, it is off by default, and it is host-only.
//
// It replaces a call to ATV_C::playerSit, which is a DEAD STUB on this build -- it writes
// a ubergraph variable nothing reads and jumps to a bare EX_PopExecutionFlow. Four runs
// called it, logged "SIT fired", and seated nobody; the acceptance's A1 arm was
// INCONCLUSIVE by its own design every time. See coop/dev/atv_probe.cpp for the seat
// verb's three gates and the five terms that decide whether the throttle produces torque.
//
// Costs nothing when off, and never walks GUObjectArray -- it registers a
// coop::element::scan_hub consumer and is HANDED its ATVs by the one shared pass.
//
// Enable with [dev] atv_probe=1 (multivoid.ini) on both peers.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::atv_probe {

// Read the ini once, register the scan-hub consumer. Idempotent; cheap when off.
// Game thread.
void Install();

// Sample on cadence and log; drive the arm when armed. Call once per net-pump tick
// (game thread). Takes the session because the arm must not fire before a peer is
// WORLD-READY: the whole point is to measure a MIRROR, and a drive window that opens
// while the other peer is still loading the save measures an ATV nobody is mirroring.
void Tick(coop::net::Session& session, bool isHost);

}  // namespace coop::dev::atv_probe
