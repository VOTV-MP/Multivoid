// coop/desk_sim_sync.h -- v111: the signal-desk download-SIM as a HOST-AUTHORITATIVE
// output stream (OPEN-0 fix; /qf-converged 2026-07-15).
//
// THE ROOT (measured): the download rate formula (AanalogDScreenTest) rolls TWO
// UNSEEDED RNG terms per tick (the detector needle DL_resDetecPercent + a transient
// `noise`) and integrates the filter offsets from per-peer frame-dt, so the OUTPUTS
// (decoded/needle/rate/frData/poData/offsets) diverge across peers even with identical
// knob inputs (host decoded 0.0064 vs client 0.0262). The old model streamed these on
// the occupant-authored, claim-gated DeskState -> unclaimed = no stream = self-diverge;
// a client occupant = client authors shared-world RNG. Seed-sync is impossible (unseeded
// + transient noise), so the fix is host-authoritative: the host owns the sim + streams
// the output vector (DeskSimPose=38, ~10Hz, newest-wins, interpolated like the cursor);
// the client OVERWRITES its own (its local sim self-accrues garbage the overwrite hides).
//
// The knob INTENTS (speeds/active/dir) stay occupant-authored on DeskState -- the host
// applies them (OnDeskState) + its own BP integrates the offset -> this vector is host-
// down ONLY (one author, gate 1). frData/poData are STREAMED (not relied on to converge
// natively: they read a filter-size upgrade with no live sync lane -- OPEN-3).
//
// One concept = one folder: lives with the other desk/device interactables.

#pragma once

namespace coop::net { class Session; }

namespace coop::desk_sim_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. HOST: read the live sim outputs + publish via
// Session::SetHostDeskSim (net thread fans out DeskSimPose). CLIENT: drain the host's
// vector, interpolate over a short LerpWindow (the cursor shape), and WriteSimOutputs
// (raw every tick for smoothness; the full screen repaint pulses at ~3Hz).
void Tick();

void OnDisconnect();

}  // namespace coop::desk_sim_sync
