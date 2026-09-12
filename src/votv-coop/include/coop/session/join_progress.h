// coop/session/join_progress.h -- the client-side join lifecycle state machine, the logic
// layer behind the loading screen, and the owner of the notice the end-reason modal shows.
// This owns the join state (the phase, the stage inside the first phase, the counts, a host
// label); the loading screen only renders a snapshot of it. The drivers: the browser connect
// actions raise Connecting; the pump forwards the session's link stage each tick; the harness's
// transfer wait loop reports the download and then the world load; the event feed's snapshot
// begin, prop spawn and snapshot complete drive Receiving to hidden; the aggregate disconnect
// resets; a master, start or transport connect failure fails. Connecting is raised only by the
// browser connect actions, never by the env or script client boot, so the loading screen is
// browser-join only. The host never enters this (the connect is gated on the client role); it
// uses the game's own native load screen, the MTA shape of a status owner and a transfer box
// reading it. Four writer contexts: the bringup, net drain, timeline (the harness join loop) and
// render threads. The timeline thread's writes race the net thread's, so its transitions are
// compare-exchanges, never blind stores: a read-then-store could re-raise a cover a concurrent
// Reset had just taken down. Atomics plus a tiny mutex for the label and the notice; no engine calls.

#pragma once

#include "coop/net/end_reason.h"

#include <cstdint>
#include <string>

namespace coop::join_progress {

enum class Phase : int {
    Idle = 0,     // no join in progress -- the cover is hidden
    // The handshake, ICE and admission, indeterminate. Usually ends at the first download note,
    // but not always: an in-gameplay client join, a host with no save, and a host that never
    // sends a begin all go straight to the snapshot from here with no download phase.
    Connecting,
    // The host's world blob is streaming in, determinate in bytes. This phase exists because it is
    // the longest part of a real join and once rendered as a connecting marquee: the effective
    // send rate on any internet link is the transport's minimum rate (no bandwidth estimation),
    // so a large world is many seconds of a screen that said nothing was happening, next to a
    // cancel button, and field joiners quit within seconds of a working download.
    Downloading,
    // The blob is in and the engine is loading it. Indeterminate: nothing reports progress out of
    // the save load, and this stage is typically half a minute to a minute and two at the cap
    // (the pump's world-ready deadline), longer than the download. Without it the cover sat on a
    // determinate bar frozen at full under the download text for that whole window, a stronger
    // hung signal than the marquee this replaced; three names for four real stages produced
    // that, and the fourth name is the fix.
    LoadingWorld,
    Receiving,    // BeginSnapshot..Complete: streaming the world, determinate bar
};

// The stages inside Connecting, each with its own sentence on the screen. Six things happen
// before the first download byte, and under one label a joiner cannot tell a slow one from a
// stuck one. The link stages come from the session's own callback edges, forwarded by the pump;
// the last comes from the save transfer's state (requested, no begin yet).
enum class Stage : int {
    None = 0,
    FindingHost,      // the server list is being asked where the host is
    Dialing,          // the transport is connecting (a direct dial, or signaling)
    FindingRoute,     // ICE is looking for a path to the host
    ProvingIdentity,  // the socket is up; the identity exchange is running
    Joining,          // admitted; the seat and the roster are landing
    WaitingForWorld,  // the host is capturing its live world for the transfer
};

// Whether the cover represents a client join or a host boot. The two share the loading-screen
// and menu-hide machinery but render different text and abort differently: a client join has
// a cancel button (stop the session and reopen the browser); a host boot has no cancel (the
// player waits for their own world) and never drives the session-stop abort path.
enum class Mode : int { Client = 0, Host = 1 };

// An immutable copy for the renderer: one cheap struct, no locks held by the caller.
struct View {
    Phase    phase = Phase::Idle;
    Stage    stage = Stage::None;  // meaningful in Connecting only
    Mode     mode = Mode::Client;
    std::string host;       // label: "Connecting to <host>" (Client) / world name (Host)
    uint32_t applied = 0;   // props applied so far (<= total)
    uint32_t total = 0;     // prop candidate total from SnapshotBegin (0 until Receiving)
    uint32_t doneBytes = 0;  // world blob received so far (0 outside Downloading)
    uint32_t totalBytes = 0; // world blob size from SaveTransferBegin (0 until it lands)
    uint64_t stageMs = 0;   // in the current phase, or stage while Connecting: the "still working"
};

// What the end-reason modal shows: the code, the site's own text beside it, and whether the
// join had completed when the link ended (the title says COULD NOT CONNECT or DISCONNECTED).
struct Notice {
    coop::net::EndReason code = coop::net::EndReason::None;
    std::string detail;
    bool afterJoin = false;
};

// Driven by the network layer, client only. `first` is the stage the action starts in: a
// browser join asks the server list first, a direct or identity dial goes straight to the transport.
void BeginConnect(const std::string& hostLabel, Stage first);  // -> Connecting (mode=Client)
void BeginSnapshot(uint32_t propTotal);            // -> Receiving (determinate)

// The stage inside Connecting moved. Forwarded by the pump each tick from the session's link
// stage; idempotent on the same value, a no-op outside Connecting, so a late report cannot
// relabel a later phase.
void NoteStage(Stage stage);

// The world-blob download moved `doneBytes` of `totalBytes`. Pulled, not pushed: the harness's
// menu-mode join loop already spins waiting on the transfer, so it polls the transfer's
// progress and forwards it here; no per-chunk work is added to the net thread, and the
// receive path keeps its single writer. A positive total is what promotes Connecting to
// Downloading; a zero total (no begin yet, or a no-save host) leaves the phase alone, so the
// marquee still covers the pre-begin window. A no-op unless a client join is in flight.
void NoteDownload(uint32_t doneBytes, uint32_t totalBytes);

// The blob is complete and the engine is about to load it: the cover goes indeterminate again
// and says so. Called by the harness the moment its transfer wait loop exits successfully,
// before the blocking world load. A no-op unless a client join is in flight in a phase this
// can legally follow.
void BeginWorldLoad();

// Driven by the harness, host only. Raise the cover for a host boot: hides the menu (so the
// player cannot wander into the browser and self-join while the world loads) and shows the
// starting-your-server text with the world name. No cancel button and no abort path; the
// harness's host-boot driver owns the lifecycle and resets this on session start or failure.
// `worldLabel` is the save or world name shown.
void BeginHostBoot(const std::string& worldLabel);  // -> Connecting (mode=Host)
void NotePropApplied();                            // ++applied (clamped); no-op unless Receiving
void Complete();                                   // -> Idle (cover lifts)
void Reset();                                       // -> Idle (force hide: disconnect/shutdown/abort)

// Abort the in-flight join. Two sources, one harness reaction (stop the session, hide the
// cover, reopen the browser), drained through TakeAbortRequest on the harness thread, since
// the render and net threads must not stop the net session directly (that joins the net
// thread): RequestCancel is the loading screen's cancel button, and Fail is a join that could
// not be established (a master or HTTP failure, a synchronous start failure, a transport
// connect that never reached connected), its code and detail logged so they surface in the
// console. Both are no-ops unless a join is active, and idempotent.
void RequestCancel();
void Fail(coop::net::EndReason code, const std::string& detail);

// The pre-flight refusal, the version gate: surface the code in the end-reason modal for a
// join rejected before the connect ever ran (no cover, no abort to drain, so unlike Fail there
// is no active gate). The lifecycle matches a fail reason: it lives until the player
// acknowledges the dialog or the next connect clears it.
void RefuseJoin(coop::net::EndReason code, const std::string& detail);

// A link the host or the transport ended once the player had a world to leave (a kick, a ban,
// the host quitting, the connection lost): the same modal. No gate, like RefuseJoin; the pump
// raises it on the aggregate-disconnect edge when the session captured a close reason, which a
// stop the player initiated never leaves. `afterJoin` says the join had completed: then the
// title is DISCONNECTED and this replaces whatever was there; a link that ended while the join
// was still in flight (the world loading or being received) fails the join instead, COULD NOT
// CONNECT with the first notice of the attempt standing, the way Fail keeps it.
void NoteDisconnect(coop::net::EndReason code, const std::string& detail, bool afterJoin);
bool TakeAbortRequest();  // true once if an abort (cancel OR fail) is pending, then clears

// The notice for the end-reason modal. Fail stashes it only when it wins the abort (a racing
// cancel that won first blocks it) and only when not shutting down; a cancel clears it
// silently. Separate from the abort flag drained by TakeAbortRequest: the notice lives until
// the player acknowledges the dialog or a new connect clears it, so the harness's stop and
// reset in the abort drain does not wipe it. The render thread peeks it each frame; the OK
// button clears it.
bool NoticePending();            // lock-free: is a modal pending? (per-frame gate)
bool PeekNotice(Notice& out);    // true + copies iff a notice is pending (takes the mutex)
void ClearNotice();              // acknowledge (hide the dialog)

// Read by the renderer, and by the pump for the stage forwarding.
bool Active();        // phase != Idle (the cover should be drawn)
Phase CurrentPhase(); // one atomic load; stages are forwarded only while Connecting
View Snapshot();      // thread-safe copy of the current state

// The failsafe, the MTA connect timeout's analogue: if a join has been active far longer than
// any real snapshot takes, log once and reset, so the player sees the game rather than a
// trapped cover. Called each frame from the render path; cheap and idempotent.
void MaybeTimeout();

}  // namespace coop::join_progress
