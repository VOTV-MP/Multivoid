// coop/dev/desk_diag.h -- read-only desk/console/dish DIVERGENCE census probe.
//
// A read-only diagnostic (RULE-2-exempt dev tool). It instruments the desk/console/computer
// divergence cluster so a holder-owned sync fix is designed from MEASURED per-peer drift rather
// than from guesses. One root: the desk's output and simulation are generated PER-PEER instead
// of being owned by the holder and mirrored, so the coordLog, the decode needle, the frequency
// and polarity filters and the dishes all drift between peers. The probe dumps every peer's
// readable desk state at a fixed cadence, plus the coordLog line delta on every change, tagged
// with role and desk-claim holder, so the host and client logs diff line by line and pin which
// field each peer self-simulates.
//
// Ini-gated `[dev] desk_diag=1` (interval `[dev] desk_diag_ms`, default 1000); zero cost when
// off, one memoized bool early-out. Game thread only -- every read dispatches to, or reads,
// engine state on the game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::desk_diag {

// Scope: four of the five divergence symptoms -- the analogDScreenTest main signal desk (its
// scalars, comp, per-dish aim targets, coordLog and caught signal) and the SAT console's LogText
// line count as a cross-surface fail-loud. The fifth, the STATIONARY PC, is NOT sampled: no reader
// has been aimed at it. Its class is laptop_C, which ue_wrap/devices/laptop.h already wraps, so
// aiming one is a matter of choosing fields rather than of finding the class. Until that happens a
// clean diff for the PC is a guaranteed false negative, not evidence of parity.

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
