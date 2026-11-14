// coop/session/world_load_episode.h -- the client world-load lifecycle: the destroy-broadcast
// suppression episode and the load-tail quiescence probe and latch, the one owner of the
// is-my-own-world-settled axis. The world-ready announce, which opens the host's per-slot send
// gate and triggers the full connect replay (the key diff, the snapshot bracket, the state
// lanes), is sent at load-tail quiescence and after the world's save replay has returned, not
// at world-up: MTA's join barrier with the save replay added, so no authoritative wire state
// ever lands in a world still churning. The episode: while a joining client is inside its own
// world load (the gamemode's pre-delete and respawn of every save object), the destroy seam
// must not broadcast the keyed-prop destroys the load churn issues; they are local net-zero
// rebuild churn, on the wire only the destroy half fires, and broadcasting it made the host
// destroy most of its authoritative copies by key on a bare join. The probe counts the load-tail population (keyless props, allowlisted NPCs, the
// chipPile field) at 5 Hz; a stable count for the required scans means the async load pass
// has drained, and an absolute ceiling bounds a pathological load, the announce firing
// anyway, loudly. The latch is a serially reused session (the join load, a travel re-seed,
// the post-snapshot sweep gate); sessions never overlap.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::world_load_episode {

// The client join world load has started (the harness timeline thread, before the blocking
// save boot). Arms the episode latch and raises the join request; the probe session opens on a
// game-thread tick once the loaded world's save replay has returned (the gamemode's
// `loadObjects`, watched by name), and HasQuiesced reads false while the request is pending, so
// the handoff window cannot leak an early announce. Idempotent. Any thread, atomics only.
void Arm();

// Open a quiescence-probe session without arming the episode: the post-snapshot sweep gate.
// Refused, logged once, while a request still awaits its world's save replay: that request's own
// session answers. Resets the latch. `reason` is logged. Game thread.
void ArmQuiesceProbe(const char* reason);

// What a pending request is: a join's world load, or a world change after the join.
enum class Request : uint8_t { None, Join, WorldChange };

// Why a request failed. WatchDead: the save-replay watch is not live (the script gate refused it,
// or the game build has no such function). ReplayTimeout: a world change's replay never returned.
enum class Failure : uint8_t { None, WatchDead, ReplayTimeout };

struct RequestFailureInfo {
    Failure failure = Failure::None;
    std::string text;  // the player-facing detail, composed where the failure is set
};

// Install the save-replay watch and keep its liveness read, on a client; a no-op on the host.
// Called every session tick, acts at most once a second, and only while a request pends or the
// watch is not yet settled. Game thread.
void InstallReplayWatch(coop::net::Session& session);

// Raise a world-change request (the pump's world-change path). True when one is pending now: this
// call raised it, or an earlier one stands; false when a join's request holds the slot, which
// covers the change. Game thread.
bool RequestWorldChangeProbe();

// True while a request awaits its world's save replay; the announce waits on it. Game thread.
bool AwaitingSaveReplay();

// The world this connection last settled on, held as a weak identity. Game thread.
// True when a baseline world is held.
bool HasBaselineWorld();
// True when `world` is the baseline: the same address holding the same live object.
bool IsBaselineWorld(void* world);
// Hold `world` as the baseline; a null `world` keeps the baseline already held.
void SetBaselineWorld(void* world);
// Forget the baseline (the connection went, or the session ended).
void ClearBaselineWorld();

// Milliseconds since a world-change request was raised, or -1 when none pends. Game thread.
int64_t PendingRequestAgeMs();

// The failure of the pending request, or None. Read every tick. Game thread.
const RequestFailureInfo& RequestFailure();

// Drive the probe. Cheap when latched or with no session open (one bool read); while a session
// is open, the 5 Hz throttled population walk, the stability counter and the ceiling. On the
// latch edge it closes the episode (if armed) and logs the reason, stable or ceiling. Returns the
// latch state; safe to call from several per-tick sites. Game thread.
bool TickQuiesceProbe();

// True once the most recent probe session latched, stable or by the ceiling, and no request awaits
// its world's save replay. Reset by the arms and by Reset. Game thread.
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

// --- the reconcile window ---
//
// A second, wider window than the episode above. The episode closes at load-tail quiescence,
// the same latch that permits the world-ready announce, and the host's snapshot bracket only
// starts after that announce, so by construction the episode always ends before the join
// reconcile it exists to cover. This one covers the whole teardown and rebuild.

// Raise the reconcile window, load kind, for a world-reload re-announce (the pump's
// world-change path; it fires on the real join flow too). Does not touch the load episode or
// the probe. Game thread.
//
// The window is raised here, at Arm and at a snapshot begin; it is lowered at the snapshot
// complete, at the sweep's lost-bracket backstop, at a rising-edge ceiling of 180 s, and at
// Reset.
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
//
// It does NOT replace InEpisode, and the lane parks (signal, email) must not follow it: a
// mid-session re-raise would absorb a legitimate local append into their re-prime, which is a
// lost write.
bool InReconcileWindow();

// Why the window was last lowered: "SnapshotComplete", "ceiling", "Reset" or "bracket-flake backstop"; empty
// before any lowering. Read-only, for a drill that must tell the snapshot's own lowering from the others.
// Game thread.
const char* LastReconcileLowerReason();

// True while the window is up with the load kind; meaningful only while InReconcileWindow.
// Any thread (atomic).
//
// The kinds: LOAD means raised through Arm or the reload arm, or a begin with no complete
// since Arm, which on the join path is the curtain never having dropped; a MID-SESSION
// bracket is a re-bracket after the first complete. The destroy seam suppresses under either
// kind -- a junk broadcast costs more than a suppressed destroy, which the bracket
// re-expresses -- while the drop intent suppresses only the load kind, since a client's
// suppressed place has no delivery channel and the re-bracket sweep would doom it.
bool ReconcileWindowIsLoadKind();

}  // namespace coop::world_load_episode
