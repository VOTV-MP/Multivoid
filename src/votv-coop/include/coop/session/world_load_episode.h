// coop/session/world_load_episode.h -- the client world-load lifecycle: the destroy-broadcast
// suppression episode and the load-tail quiescence probe and latch, the one owner of the
// is-my-own-world-settled axis. The world-ready announce, which opens the host's per-slot send
// gate and triggers the full connect replay (the key diff, the snapshot bracket, the state
// lanes), is sent at load-tail quiescence rather than at world-up, the MTA join barrier, so
// no authoritative wire state ever lands in a world still churning. The episode: while a
// joining client is inside its own world load (the gamemode's pre-delete and respawn of every
// save object), the destroy seam must not broadcast the keyed-prop destroys the load churn
// issues; they are local net-zero rebuild churn, on the wire only the destroy half fires, and
// broadcasting it made the host destroy most of its authoritative copies by key on a bare
// join. The probe counts the load-tail population (keyless props, allowlisted NPCs, the
// chipPile field) at 5 Hz; a stable count for the required scans means the async load pass
// has drained, and a two-tier deadline bounds a pathological load, the announce firing
// anyway, loudly. The latch is a serially reused session (the join load, a travel re-seed,
// the post-snapshot sweep gate); sessions never overlap.

#pragma once

namespace coop::world_load_episode {

// The client join world load has started (the harness timeline thread, before the blocking
// save boot). Arms the episode latch and raises the join probe-session request; the session
// opens on the next game-thread probe tick, and HasQuiesced reads false while the request is
// pending, so the handoff window cannot leak an early announce. Idempotent. Any thread,
// atomics only.
void Arm();

// Open a quiescence-probe session without arming the episode: the travel re-announce (the new
// world must settle before the host re-replays into it) and the post-snapshot sweep gate.
// Resets the latch. `reason` is logged. Game thread.
void ArmQuiesceProbe(const char* reason);

// Drive the probe. Cheap when latched or with no session open (one bool read); while a session
// is open, the 5 Hz throttled population walk, the stability counter and the two-tier
// deadline. On the latch edge it closes the episode (if armed) and logs the reason, stable or
// deadline. Returns the latch state; safe to call from several per-tick sites. Game thread.
bool TickQuiesceProbe();

// True once the most recent probe session latched, stable or by deadline. Reset by the arms
// and by Reset. Game thread.
bool HasQuiesced();

// Milliseconds since the latch, or -1 when not latched; the sweep's post-announce
// bracket-timeout backstop uses it. Game thread.
long long MsSinceQuiesced();

// Session teardown: clear the episode, the probe session and the latch. Game thread.
void Reset();

// True while a client world-load episode is in progress; the destroy seam suppresses
// keyed-prop destroy broadcasts while it holds. Any thread (a relaxed atomic; advisory for
// the census tag).
bool InEpisode();

// The reconcile window. The episode above closes at load-tail quiescence, the same latch that
// permits the world-ready announce, and the host's snapshot bracket only starts after the
// announce, so by construction the episode always ends before the join reconcile it exists to
// cover. This second window covers the whole teardown and rebuild: raised at Arm, at the
// world-reload re-announce arm and at a snapshot begin; lowered at the snapshot complete, at
// the sweep's lost-bracket backstop, at a rising-edge ceiling of 180 s, and at Reset. It does
// not replace InEpisode: the lane parks (signal, email) must not follow it, since a
// mid-session re-raise would absorb a legitimate local append into their re-prime, a lost
// write. The kind: load means raised through Arm or the reload arm, or a begin with no
// complete since Arm, which on the join path is the curtain never having dropped; a
// mid-session bracket is a re-bracket after the first complete. The destroy seam suppresses
// under any kind (a junk broadcast costs more than a suppressed destroy, which the bracket
// re-expresses); the drop intent suppresses only the load kind, since a client's suppressed
// place has no delivery channel and the re-bracket sweep would doom it.

// Raise the reconcile window, load kind, for a world-reload re-announce (the pump's
// world-change path; it fires on the real join flow too). Does not touch the load episode or
// the probe. Game thread.
void RaiseReconcileForReload();

// A client snapshot begin was received. With the window up, refresh (the kind kept, no
// ceiling restamp); down, raise it, the kind being mid-session-bracket if a complete happened
// since Arm and load otherwise. Game thread.
void NoteReconcileBegin();

// A client snapshot complete was received. Unconditionally records complete-since-Arm (a
// post-ceiling complete keeps the classifier right), then lowers the window if up. Game
// thread.
void NoteReconcileComplete();

// The sweep's lost-bracket backstop fired (no snapshot begin within its timeout of the
// announce). Lowers the window without touching the classifier, so a late real bracket's begin
// still classifies as load. Game thread.
void NoteBracketFlake();

// True while the reconcile window is up. Any thread (atomic).
bool InReconcileWindow();

// True while the window is up with the load kind; meaningful only while InReconcileWindow.
// Any thread (atomic).
bool ReconcileWindowIsLoadKind();

}  // namespace coop::world_load_episode
