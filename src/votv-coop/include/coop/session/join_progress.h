// coop/join_progress.h -- CLIENT-side join lifecycle state machine (the logic
// layer behind the loading screen).
//
// Principle 7 split: this owns the join STATE (phase + progress counts + a host
// label); ui/loading_screen.cpp only RENDERS a Snapshot() of it. The network
// layer drives it -- the client process calls these as the join unfolds:
//
//   session_manager Join/ConnectDirect (BROWSER only) -> BeginConnect(host) [Connecting]
//   harness  save-transfer wait loop (poll)  -> NoteDownload(done,total) [Downloading]
//   harness  transfer done, before the load  -> BeginWorldLoad()      [LoadingWorld]
//   event_feed  ReliableKind::SnapshotBegin -> BeginSnapshot(total)   [Receiving]
//   event_feed  ReliableKind::PropSpawn      -> NotePropApplied()     (fills the bar)
//   event_feed  ReliableKind::SnapshotComplete -> Complete()          [hidden]
//   net_pump aggregate disconnect / shutdown -> Reset()               [hidden]
//   master/Start/GNS connect failure        -> Fail(reason)           [abort -> hidden]
//
// Two of those are driven by the HARNESS, not the network layer: the transfer's
// progress is PULLED off save_transfer by the loop that is already waiting on it
// (see NoteDownload), which is why the list above is no longer purely net-driven.
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
// Thread-safety: FOUR writer contexts, not three. BeginConnect runs on the bringup
// thread; BeginSnapshot / NotePropApplied / Complete run on the net-message-drain
// thread; NoteDownload / BeginWorldLoad run on the TIMELINE thread (the harness join
// loop); Snapshot() / MaybeTimeout() run on the render thread. Because the timeline
// thread's writes race the net thread's, the phase transitions it performs are
// compare-exchanges, never blind stores -- a read-then-store there could re-raise a
// cover a concurrent Reset() had just taken down. State is atomics + a tiny mutex for
// the host label. No engine calls -- pure state.

#pragma once

#include <cstdint>
#include <string>

namespace coop::join_progress {

enum class Phase : int {
    Idle = 0,     // no join in progress -- the cover is hidden
    // Handshake/ICE/admission, indeterminate. Usually ends at the first NoteDownload,
    // but NOT always: an in-gameplay (idleInGameplay) client join, a host with no
    // save, and a host that never sends Begin all go straight to BeginSnapshot from
    // here without a download phase at all.
    Connecting,
    // The host's world blob is streaming in -- determinate, in BYTES. This phase
    // exists because it is the LONGEST part of a real join and used to render as
    // "Connecting..." with a marquee: the effective send rate on any internet link
    // is SendRateMin = 1 MB/s (no bandwidth estimation in this build --
    // session_status.cpp:72-79), so a 17.6 MB world is ~17 s of a screen that said
    // nothing was happening, next to a Cancel button. Two field joiners quit at
    // +2 s and +3 s on 2026-09-02, ~15% in; the ONLY producers of the close reason
    // they sent are that button and closing the game.
    Downloading,
    // The blob is in and the ENGINE is loading it. Indeterminate: nothing reports
    // progress out of LoadStorySave, and this stage is 30-60 s typically and 120 s
    // at the cap (net_pump.cpp's world-ready deadline) -- LONGER than the download.
    // It exists because without it the cover sat on a determinate bar FROZEN AT
    // 100% under "Downloading the host's world" for that whole window, which is a
    // stronger "hung" signal than the marquee this change set out to replace. Three
    // names for four real stages is what produced that; the fourth name is the fix
    // (post-ship audit, 2026-09-02).
    LoadingWorld,
    Receiving,    // BeginSnapshot..Complete: streaming the world, determinate bar
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
    uint32_t doneBytes = 0;  // world blob received so far (0 outside Downloading)
    uint32_t totalBytes = 0; // world blob size from SaveTransferBegin (0 until it lands)
    uint64_t elapsedMs = 0; // since BeginConnect (for the failsafe + a subtle "still working")
};

// --- Driven by the network layer (client only) ---------------------------------
void BeginConnect(const std::string& hostLabel);  // -> Connecting (mode=Client)
void BeginSnapshot(uint32_t propTotal);            // -> Receiving (determinate)

// The world-blob download moved `doneBytes` of `totalBytes`. PULLED, not pushed:
// the harness's menu-mode join loop already spins at ~60 Hz waiting on the transfer
// (session_runtime DriveMenuModeJoinWorldBoot), so it polls save_transfer::GetProgress
// and forwards it here -- no per-chunk work is added to the net thread, and the
// receive path keeps its single writer. `totalBytes > 0` is what promotes Connecting
// -> Downloading; a zero total (no Begin yet, or a no-save host) leaves the phase
// alone, so the indeterminate marquee still covers the pre-Begin window. No-op unless
// a CLIENT join is in flight.
void NoteDownload(uint32_t doneBytes, uint32_t totalBytes);

// The blob is complete and the engine is about to load it -- the cover goes
// indeterminate again and says so. Called by the harness the moment its transfer
// wait loop exits successfully, BEFORE the blocking world load. No-op unless a
// CLIENT join is in flight and the phase is one this can legally follow.
void BeginWorldLoad();

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

// PRE-FLIGHT refusal (v122 version gate): surface `reason` in the connect-failed
// dialog for a join that was rejected BEFORE BeginConnect ever ran (no cover, no
// abort to drain -- unlike Fail there is no Active() gate because nothing is in
// flight). Lifecycle identical to a Fail reason: lives until the user OKs the
// dialog (ClearFailReason) or the next BeginConnect clears it.
void RefuseJoin(const std::string& reason);
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
