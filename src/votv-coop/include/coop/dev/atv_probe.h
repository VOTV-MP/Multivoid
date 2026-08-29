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
// THE SIT ARM ([dev] atv_probe_sit=1, HOST only, one shot).
// Sampling an IDLE ATV measures nothing about a MIRROR, because an idle ATV is never
// mirrored: atv_sync.cpp:453 releases an unauthored one and no peer streams it. So the
// open question -- docs/vehicles/ATV.md 11.1, do a mirror's wheels follow its body --
// needs the ATV AUTHORED, which needs an occupant. This arm calls the game's own
// ATV_C::playerSit(player) with the local player once, ~25 s in, which is the same
// path pressing USE walks. The host then becomes the authority, streams, and the
// client builds a real mirror to measure. This is the ONLY write the probe makes, it
// is off by default, and it is host-only.
//
// Costs nothing when off, and never walks GUObjectArray -- it registers a
// coop::element::scan_hub consumer and is HANDED its ATVs by the one shared pass.
//
// Enable with [dev] atv_probe=1 (multivoid.ini) on both peers.

#pragma once

namespace coop::dev::atv_probe {

// Read the ini once, register the scan-hub consumer. Idempotent; cheap when off.
// Game thread.
void Install();

// Sample on cadence and log; drive the sit arm when armed. Call once per net-pump
// tick (game thread).
void Tick(bool isHost);

}  // namespace coop::dev::atv_probe
