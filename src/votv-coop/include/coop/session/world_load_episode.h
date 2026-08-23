// coop/session/world_load_episode.h -- the client world-load lifecycle: the destroy-broadcast
// suppression EPISODE + the load-tail QUIESCENCE probe/latch. ONE owner of the "is my own world
// settled" axis ([[feedback-one-owner-order-axis]]).
//
// JOIN-BARRIER REDESIGN 2026-07-12 (research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md):
// ClientWorldReady -- the announce that opens the host's per-slot send gate and triggers the full
// connect replay (R2 key-diff -> snapshot bracket -> state lanes) -- is now sent at LOAD-TAIL
// QUIESCENCE, not at "world up + registry coherent". This is the MTA join barrier
// (INITIAL_DATA_STREAM: the client says READY only when fully loaded, and only then does the server
// stream the world -- reference/mtasa-blue Server/.../CGame.cpp:1393, Client/.../CPacketHandler.cpp:463).
// Consequence: NO authoritative wire state ever lands in a world that is still churning, which
// dissolves the two-authority join seam (RCA roots 1/2/4/5) at the source instead of compensating
// for it (the retired spawn-revalidation capture + wire-order netting layers).
//
// THE EPISODE (v107 host-wipe fix, unchanged role): while a joining CLIENT is inside its own
// world-load (the game's mainGamemode.loadObjects pre-delete + respawn), the v106 K2_DestroyActor
// destroy seam must NOT broadcast the KEYED-prop destroys the load-churn issues. Those destroys are
// LOCAL net-zero rebuild churn; on the wire only the destroy half fires, and broadcasting it makes
// the HOST destroy its authoritative copies by key (measured 2026-07-08: a bare client join drove
// the host 3345 -> 1255 live keyed props). Why a latch and not the alternatives (caller gate /
// authority-default-deny / recreate-pairing / mark-the-victims): vetted /qf rounds 0-13 2026-07-08,
// see research/findings/props-lifecycle/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md.
//
// THE QUIESCENCE PROBE (moved here 2026-07-12 from join_membership_sweep, which now CONSUMES it):
// counts the load-tail population -- keyless not-yet-key-minted props + allowlisted NPCs + the
// purge-aware chipPile field (docs/piles/10) -- at 5 Hz; when the count is stable for
// kQuiesceStableScans the async loadObjects pass has drained. Two-tier deadline (no-progress 45 s /
// absolute 120 s) bounds a pathological load: on a deadline latch the announce fires anyway (LOUD --
// the degraded mode; the steady drain + rejoin cover what a settled-world join would have gotten
// right). The probe LATCH is a serially-reused session:
//   session 1: Arm() (join world-load)            -> latch closes the episode + gates the announce
//   session 2: ArmQuiesceProbe (travel re-seed)   -> gates the RE-announce (net_pump maybeReAnnounce)
//   session 3: ArmQuiesceProbe (post-snapshot)    -> gates the divergence sweep fire edge
// Sessions never overlap (each consumer arms only after the prior latched by construction).
// The old SnapshotBegin-lost watchdog chain (150 s episode ceiling -> sweep declares
// quiescence-by-ceiling) is RETIRED: the episode no longer depends on any wire message to close
// (the probe is client-local), and the lost-bracket flake is covered by the sweep's own
// post-announce bracket timeout (join_membership_sweep.cpp).
//
// LIFECYCLE (game-thread only EXCEPT Arm; the episode flag + the join-probe request are atomics):
//   Arm()             -- harness TimelineThread, immediately before BootStorySaveBlocking (client
//                        join path). OFF-GT SAFE BY CONTRACT: touches only atomics -- it arms the
//                        episode and RAISES the join probe-session request; the session itself
//                        opens on the next GT TickQuiesceProbe (audit CRITICAL 2026-07-12: opening
//                        it inline raced the GT ticker on the plain probe fields). HasQuiesced()
//                        reads false while the request is pending, so the handoff window cannot
//                        leak an early announce.
//   ArmQuiesceProbe() -- (re)open a probe session without arming the episode (travel re-announce,
//                        post-snapshot sweep gate). Resets the latch.
//   TickQuiesceProbe()-- drive per client tick (net_pump + the sweep both call it; internal 5 Hz
//                        throttle + latched early-out make double-driving free). Returns the latch.
//   HasQuiesced()     -- the latch of the most recent session.
//   MsSinceQuiesced() -- ms since the latch (-1 when not latched). The sweep's flake backstop.
//   InEpisode()       -- the destroy seam's suppress query (prop_lifecycle::DestroySeamBody).
//   Reset()           -- session teardown (join_membership_sweep::ResetClaimTracking).
//
// [[feedback-folder-per-domain-concept-rule]] -- moved coop/props/ -> coop/session/ 2026-07-12: the
// concept is the client WORLD-LOAD lifecycle (it gates the session announce, the destroy seam, NPC
// adoption and the sweep), not a prop behavior.

#pragma once

namespace coop::world_load_episode {

// The client join world-load has started (harness TimelineThread, before BootStorySaveBlocking).
// Arms the episode latch and raises the join probe-session request (the session opens on the next
// GT TickQuiesceProbe). Idempotent. ANY thread (atomics only).
void Arm();

// (Re)open a quiescence-probe session WITHOUT arming the episode: the travel re-announce
// (net_pump::maybeReAnnounce -- the new world must settle before the host re-replays into it) and
// the post-snapshot sweep gate (join_membership_sweep::ArmDivergenceSweep). Resets the latch.
// `reason` is logged. Game thread.
void ArmQuiesceProbe(const char* reason);

// Drive the probe. Cheap when latched or no session is open (one bool read); while a session is
// open: 5 Hz throttled population walk + stability counter + the two-tier deadline. On the latch
// edge closes the episode (if armed) and logs the reason (stable vs deadline). Returns the latch
// state. Safe to call from multiple per-tick sites. Game thread.
bool TickQuiesceProbe();

// True once the most recent probe session latched (stable or deadline). Reset by Arm /
// ArmQuiesceProbe / Reset. Game thread.
bool HasQuiesced();

// Milliseconds since the latch, or -1 when not latched. Used by the sweep's post-announce
// bracket-timeout flake backstop. Game thread.
long long MsSinceQuiesced();

// Session teardown -- clear the episode + the probe session + the latch. Game thread.
void Reset();

// True while a client world-load episode is in progress. The destroy seam suppresses KEYED-prop
// destroy broadcasts while this holds. Any thread (atomic relaxed; advisory for the census tag).
bool InEpisode();

// ---- The RECONCILE WINDOW (R-4a end-condition, 2026-08-23; 9-round /qf) -------------------
// Design of record: research/findings/architecture-audits/votv-r4a-end-condition-DESIGN-2026-08-23.md.
// The episode above closes at load-tail quiescence -- the SAME latch that permits the
// ClientWorldReady announce, and the host's snapshot bracket only starts AFTER the announce: by
// construction the episode always ends before the join reconcile it exists to cover (the field's
// 1,629 junk destroy broadcasts landed 23 s after "episode CLOSED"). This SECOND window covers
// the whole teardown-AND-rebuild: raised at Arm() / the world-reload re-announce arm /
// SnapshotBegin; lowered at SnapshotComplete / the sweep's lost-bracket flake backstop / a 180 s
// rising-edge ceiling / Reset. It does NOT replace InEpisode() -- the lane parks (signal/email)
// measurably must NOT follow it (a mid-session re-raise would absorb a legitimate local append
// into their re-prime = a lost write).
//
// KIND: `load` = raised via Arm()/reload-arm, OR a Begin with NO SnapshotComplete since Arm --
// on the join path that is exactly "the curtain never dropped" (the player could act only
// BLINDLY; the curtain is render-gate-only, imgui_overlay.cpp:548-551). `midSessionBracket` = a
// re-bracket after the first Complete (the player is live). Readers: the destroy seam suppresses
// under ANY kind (a junk broadcast costs more than a suppressed destroy, which the bracket
// re-expresses); drop-intent suppresses ONLY kind==load (a client's suppressed place has NO
// delivery channel -- the census express is host-only -- and the re-bracket sweep would doom it;
// genuine mid-session places must flow intent->host-spawn->express->claim).

// Raise the reconcile window kind=load for a world-reload re-announce (net_pump's world-change
// path; fires on the REAL join flow too). Does NOT touch the load episode or the probe. GT.
void RaiseReconcileForReload();

// Client SnapshotBegin received (event_feed GT drain). Window up -> refresh (kind kept, no
// ceiling restamp); down -> raise, kind = (a Complete happened since Arm) ? midSessionBracket
// : load. GT.
void NoteReconcileBegin();

// Client SnapshotComplete received. UNCONDITIONALLY records complete-since-Arm (a post-ceiling
// Complete keeps the classifier right), then lowers the window if up. GT.
void NoteReconcileComplete();

// True while the reconcile window is up. Any thread (atomic).
bool InReconcileWindow();

// True while the window is up with kind==load. Meaningful only while InReconcileWindow(). Any
// thread (atomic).
bool ReconcileWindowIsLoadKind();

}  // namespace coop::world_load_episode
