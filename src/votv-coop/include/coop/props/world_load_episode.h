// coop/props/world_load_episode.h -- the client world-load destroy-broadcast suppression episode.
//
// ONE concept: while a joining CLIENT is inside its own world-load (the game's mainGamemode.loadObjects
// pre-delete + respawn), the v106 K2_DestroyActor destroy seam must NOT broadcast the KEYED-prop destroys
// that the load-churn issues. Those destroys are LOCAL, net-zero world-rebuild churn -- the game tears down
// and re-creates every keyed prop same-key, and the client re-binds each via join_membership_sweep. On the
// wire, however, only the destroy half fires the seam: broadcasting it makes the HOST destroy its
// AUTHORITATIVE copies by key. Measured 2026-07-08: a BARE client join (zero player action) drove the host
// from 3345 -> 1255 live keyed props and never recovered -- the host world emptied. This is a v106
// regression: pre-v106 the churn dispatched via EX_* (ProcessEvent-invisible) and never crossed the wire;
// three pre-v106 join runs ran the identical purge and broadcast ZERO. The v106 Func-patch is what first put
// the load-churn destroys on the wire. See research/findings/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md
//
// WHY AN EPISODE LATCH, not the alternatives (vetted /qf rounds 0-13, 2026-07-08):
//   - A per-destroy FFrame::Node caller gate (skip when the caller UFunction is loadObjects/Load
//     Primitives/...) is a SITE LIST -- it leaks if the churn-UFunction set is incomplete (OPUS_48 §9:150).
//   - Authority-default-deny (client never broadcasts a keyed destroy it did not author via intent) must
//     RE-ALLOW the synth-keyed pile morphs that legitimately broadcast keyed through this same seam ->
//     touches the frozen "piles already fixed" path.
//   - Recreate-pairing (broadcast only if no same-key recreate follows) has NO successor at destroy time on
//     a bare join (the recreate arrives ~7 s later via the join_membership_sweep scan-rebind, not a spawn),
//     and mis-adjudicates synth-keyed morphs.
//   - Mark-the-victims (like GHOST-RETIRE/mass-purge pre-suppress) is INFEASIBLE: the victims are born
//     AFTER the arm (adopted during the join travel) and destroyed INSIDE loadObjects, which dispatches via
//     EX_LocalFinalFunction and is un-hookable (net_pump.cpp:987). There is no moment we can mark them.
// The episode latch is the source-anchored INVARIANT ("am I inside my own world-load"): it needs neither to
// enumerate the churn UFunctions nor to mark the victims, and it covers every in-window churn issuer.
//
// LIFECYCLE (all game-thread only; no mutex):
//   Arm()           -- the client join world-load trigger (harness, immediately before BootStorySaveBlocking).
//                      CAUSALLY before the burst: the boot triggers loadObjects, whose pre-delete IS the burst.
//                      Client-only call site (the host boots its world through a different path).
//   NotifyQuiesced()-- join_membership_sweep drives this at load-tail quiescence (g_sweepFired = true), the
//                      SAME bounded edge (deadline-capped at 45 s) that ends the divergence-sweep window. The
//                      measured window [arm .. quiescence] brackets the wipe burst; the only legit post-load
//                      keyed intent destroys (food morph, trash-container E-press, rock R-pickup) fire AFTER
//                      quiescence and are broadcast normally.
//   Reset()         -- session teardown (join_membership_sweep::ResetClaimTracking).
//   InEpisode()     -- the destroy seam (prop_lifecycle::DestroySeamBody) queries this to suppress the
//                      OUTBOUND broadcast of KEYED destroys. eid-only (pile) destroys are NOT gated here.
//
// [[feedback-folder-per-domain-concept-rule]] [[feedback-join-reconcile-sweep-safety]]

#pragma once

namespace coop::world_load_episode {

// The client join world-load has started (harness, before BootStorySaveBlocking). Idempotent. Game thread.
void Arm();

// Load-tail quiescence reached -- end the episode (join_membership_sweep at g_sweepFired). Game thread.
void NotifyQuiesced();

// Session teardown -- clear the episode unconditionally. Game thread.
void Reset();

// True while a client world-load episode is in progress. The destroy seam suppresses KEYED-prop destroy
// broadcasts while this holds. Game thread.
bool InEpisode();

}  // namespace coop::world_load_episode
