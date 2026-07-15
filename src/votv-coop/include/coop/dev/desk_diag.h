// coop/dev/desk_diag.h -- read-only desk/console/dish DIVERGENCE census probe.
//
// READ-ONLY diagnostic (RULE-2-exempt, dev-tool). Instruments the 2026-07-15
// desk/console/computer DIVERGENCE CLUSTER so the holder-owned sync fix is
// designed from MEASURED per-peer drift, not guesses (PROBE-DON'T-GUESS,
// [[feedback-probe-dont-guess-rule]]). ONE root: the desk OUTPUT + SIMULATION
// is generated PER-PEER, not owned by the holder/host and mirrored -- so the
// coordLog (host 78 lines vs client 13), the decode needle (017.77 vs 017.84),
// the freq/polarity filters (unsynced), and the dishes drift between peers.
// This probe dumps every peer's full readable desk state at a fixed cadence +
// the coordLog line-delta on every change, tagged role + desk-claim holder, so
// the HOST and CLIENT logs diff line-by-line and pin exactly which field each
// peer self-simulates.
//
// Scope: covers 4 of the 5 divergence symptoms -- the analogDScreenTest main
// signal desk (scalars incl. DL_frData/DL_poData / comp / per-dish aim targets /
// coordLog / caught-signal) + the SAT-console LogText line-count (cross-surface
// fail-loud). The 5th symptom -- the STATIONARY PC -- is NOT covered: its device
// class is UNRESOLVED. Alaptop_C was ruled OUT by reflection (it is a GRABBABLE
// portable laptop: canPickup/playerGrabbed, not the system-block-powered
// stationary PC); the real class (candidates pcWASDtest / prop_computerpanels)
// needs its own surface-resolution pass before a reader is aimed. Until then this
// instrument is 4/5 -- it does NOT sample the stationary PC and a clean diff there
// is a guaranteed false negative, NOT evidence of parity.
//
// Ini-gated `[dev] desk_diag=1` (interval `[dev] desk_diag_ms`, default 1000);
// zero cost when off (one memoized bool early-out). Game thread only (every CD::
// read dispatches / reads engine state on the game thread).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::desk_diag {

// Memoized `[dev] desk_diag` ini flag.
bool IsEnabled();

// Cache the live session (for the role + running() gate). Called from the
// subsystems Install fanout. No-op behaviour when the ini flag is off.
void Install(coop::net::Session* session);

// Periodic snapshot + coordLog-change dump. Game thread; call every pump tick
// (self-throttled to `desk_diag_ms`; single bool read when disabled).
void Tick();

// JOIN-ADOPT hook: call from console_state_sync::OnDeskState's adopt branch,
// BEFORE WriteScalars mutates local state, so the probe captures the client's
// PRE-ADOPT desk scalars (the join baseline for the adopted set). The delta vs
// the next post-adopt periodic snapshot = a JOIN-SEED divergence (peers wrong
// from t0); agreement then later drift = RUNTIME drift. No-op when the ini flag
// is off. Game thread. (coordLog is session-local -> NOT adopted; its baseline
// is the empty prime, tagged separately -- see the .cpp.)
void NoteJoinAdopt();

}  // namespace coop::dev::desk_diag
