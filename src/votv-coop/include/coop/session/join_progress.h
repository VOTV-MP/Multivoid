// coop/join_progress.h -- CLIENT-side join lifecycle state machine (the logic
// layer behind the loading screen).
//
// Principle 7 split: this owns the join STATE (phase + progress counts + a host
// label); ui/loading_screen.cpp only RENDERS a Snapshot() of it. The network
// layer drives it -- the client process calls these as the join unfolds:
//
//   session_manager Join/ConnectDirect (BROWSER only) -> BeginConnect(host) [Connecting]
//   event_feed  ReliableKind::SnapshotBegin -> BeginSnapshot(total)   [Receiving]
//   event_feed  ReliableKind::PropSpawn      -> NotePropApplied()     (fills the bar)
//   event_feed  ReliableKind::SnapshotComplete -> Complete()          [hidden]
//   net_pump aggregate disconnect / shutdown -> Reset()               [hidden]
//   master/Start/GNS connect failure        -> Fail(reason)           [abort -> hidden]
//
// BeginConnect is raised ONLY by the browser connect actions (session_manager), never
// by the env/.bat/autotest client boot (which calls the harness StartCoopSession
// directly) -- so the loading screen is BROWSER-JOIN-ONLY (regression A, 2026-06-06).
//
// HOST never enters this (BeginConnect is gated role==Client) -- the host uses
// VOTV's own native load screen for its save load. MTA shape: CClientGame owns
// m_Status (CONNECTING->JOINING->JOINED) and tells CTransferBox; here join_progress
// owns the phase and loading_screen reads it.
//
// Thread-safety: BeginConnect runs on the bringup thread; BeginSnapshot /
// NotePropApplied / Complete run on the net-message-drain thread; Snapshot() /
// MaybeTimeout() run on the render thread. State is atomics + a tiny mutex for the
// host label. No engine calls -- pure state.

#pragma once

#include <cstdint>
#include <string>

namespace coop::join_progress {

enum class Phase : int {
    Idle = 0,    // no join in progress -- the cover is hidden
    Connecting,  // BeginConnect..BeginSnapshot: handshake/ICE, indeterminate
    Receiving,   // BeginSnapshot..Complete: streaming the world, determinate bar
};

// Whether the cover represents a CLIENT join or a HOST boot. The two share the
// loading-screen + menu-hide machinery (Active()) but render different text and
// have different abort semantics: a client join has a Cancel button (-> stop the
// session + reopen the browser); a host boot has NO cancel (the user waits for
// their own world to load) and never drives the session-stop abort path.
enum class Mode : int { Client = 0, Host = 1 };

// Immutable copy for the renderer (one cheap struct, no locks held by the caller).
struct View {
    Phase    phase = Phase::Idle;
    Mode     mode = Mode::Client;
    std::string host;       // label: "Connecting to <host>" (Client) / world name (Host)
    uint32_t applied = 0;   // props applied so far (<= total)
    uint32_t total = 0;     // prop candidate total from SnapshotBegin (0 until Receiving)
    uint64_t elapsedMs = 0; // since BeginConnect (for the failsafe + a subtle "still working")
};

// --- Driven by the network layer (client only) ---------------------------------
void BeginConnect(const std::string& hostLabel);  // -> Connecting (mode=Client)
void BeginSnapshot(uint32_t propTotal);            // -> Receiving (determinate)

// --- Driven by the harness (host only) -----------------------------------------
// Raise the cover for a HOST boot (the Host-Game flow): hides the menu (so the
// user can't wander into the browser + self-join while the world loads) and shows
// "Starting your server -- loading <world>". NO Cancel button + no abort path (the
// harness DriveHostBootIfPending owns the lifecycle and Reset()s this on session
// start / failure). `worldLabel` is the save/world name shown to the user.
void BeginHostBoot(const std::string& worldLabel);  // -> Connecting (mode=Host)
void NotePropApplied();                            // ++applied (clamped); no-op unless Receiving
void Complete();                                   // -> Idle (cover lifts)
void Reset();                                       // -> Idle (force hide: disconnect/shutdown/abort)

// Abort the in-flight join. TWO sources, ONE harness reaction (Stop the session + hide
// the cover + reopen the browser), drained via TakeAbortRequest on the harness thread
// (the render/net threads must not Stop the net session directly -- that joins the net
// thread):
//   * RequestCancel -- the loading screen's "Cancel" button (render thread, user-asked).
//   * Fail          -- the join could not be established: a master/HTTP failure, a
//                      synchronous Start() failure, or a GNS connect that never reached
//                      Connected (dead address / unreachable host). `reason` is logged
//                      (WARN) so it surfaces in the console. Both are no-ops unless a join
//                      is Active, and idempotent (re-firing until the harness drains the
//                      abort is harmless).
void RequestCancel();
void Fail(const std::string& reason);
bool TakeAbortRequest();  // true once if an abort (cancel OR fail) is pending, then clears

// Connect-failure reason (for ui/connect_failed_dialog). Fail() stashes `reason`
// ONLY when it wins the abort (a racing user Cancel that won first blocks it) and
// only when not shutting down; a user Cancel clears it (silent). This is SEPARATE
// from the abort flag drained by TakeAbortRequest -- the reason lives until the
// user acknowledges the dialog (ClearFailReason) or a new BeginConnect clears it,
// so the harness's Stop+Reset in the abort drain does NOT wipe it. Render-thread
// reads via PeekFailReason each frame; the "OK" button calls ClearFailReason.
bool FailPending();                     // lock-free: is a failure modal pending? (per-frame gate)
bool PeekFailReason(std::string& out);  // true + copies iff a failure is pending (takes the mutex)
void ClearFailReason();                 // acknowledge (hide the dialog)

// --- Read by the renderer ------------------------------------------------------
bool Active();        // phase != Idle (the cover should be drawn)
View Snapshot();      // thread-safe copy of the current state

// Failsafe (MTA NET_CONNECT_TIMEOUT analogue): if a join has been Active far longer
// than any real snapshot takes, log once + Reset so the user sees the game rather
// than a trapped cover. Called each frame from the render path; cheap + idempotent.
void MaybeTimeout();

}  // namespace coop::join_progress
