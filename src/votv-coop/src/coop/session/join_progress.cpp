// coop/session/join_progress.cpp -- see coop/session/join_progress.h.

#include "coop/session/join_progress.h"

#include "coop/net/protocol.h"  // HostJoinPhase -- naming what the host said it was doing
#include "ui/join_curtain.h"  // drop the curtain on a join abort (not the normal complete path)
#include "coop/session/rig_ready.h"
#include "coop/session/shutdown.h"  // IsShuttingDown -- suppress the failure dialog during teardown
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace coop::join_progress {
namespace {

using coop::net::Describe;
using coop::net::EndReason;

// The phase and the counts are atomics (written on the net-drain thread, read on the render
// thread); the host label is a string under its own mutex (written rarely in BeginConnect,
// read once a frame in Snapshot).
std::atomic<int>      g_phase{static_cast<int>(Phase::Idle)};
std::atomic<int>      g_stage{static_cast<int>(Stage::None)};
std::atomic<int>      g_mode{static_cast<int>(Mode::Client)};
std::atomic<uint32_t> g_applied{0};
std::atomic<uint32_t> g_total{0};
// World-blob download counters (bytes). Written by the harness join loop via NoteDownload,
// read on the render thread in Snapshot.
std::atomic<uint32_t> g_dlDone{0};
std::atomic<uint32_t> g_dlTotal{0};
std::atomic<int64_t>  g_startMs{0};
// When the current phase, or the current stage inside Connecting, began: the screen shows the
// seconds spent in it once they add up, so a step that is merely slow reads as still working.
std::atomic<int64_t>  g_stageStartMs{0};
// When this phase's token last advanced: a received byte, an applied prop, a host beacon, or the
// phase itself beginning. The watchdog reads the age of this and nothing else.
std::atomic<int64_t>  g_tokenMs{0};
// The host's last beacon: its phase for the screen, its numerator so a bracket that is streaming
// renews the wait even while this client applies nothing (every candidate can be a skip).
std::atomic<uint8_t>  g_hostPhase{0};
std::atomic<uint32_t> g_hostDone{0};
std::atomic<uint32_t> g_hostTotal{0};
std::atomic<uint32_t> g_hostBeacons{0};  // beacons heard this attempt; the join's closing line reports it
// When the last beacon ARRIVED. Not a failure trigger -- only the word the failure line uses, so a
// report can tell a host that went quiet from one that answered all along and got nowhere.
std::atomic<int64_t>  g_hostBeaconMs{0};
// Which of the announce gates this client is held on, and what it was when last reported. The
// watchdog reads the phase token; this only names the gate in the failure sentence.
std::atomic<uint8_t>  g_worldGate{0};
std::atomic<bool>     g_abortReq{false};  // Cancel button OR a connect failure -> harness drains (Stop + reopen browser)

std::mutex  g_hostMu;
std::string g_host;  // guarded by g_hostMu

// The notice for the end-reason modal (see the header). Set by the abort winner (Fail stashes,
// Cancel clears), by a pre-flight refusal, or by a close after the join; read and cleared by the
// render thread; its own mutex, independent of the abort flag (which the harness drains
// separately).
std::mutex g_noticeMu;
Notice     g_notice;  // guarded by g_noticeMu; code != None == a modal is pending
// A lock-free mirror of "a notice is pending" so the per-frame overlay gate never takes the
// mutex; only the render's actual read does. Kept in sync inside the mutex's critical sections.
std::atomic<bool> g_noticePending{false};

// The host-boot failsafe backstop. The harness's host-boot drive normally resets this on
// session start or its own timeout; this longer cap fires only if that path never ran (a
// stuck cover at the menu is worse than dropping it). Host mode just resets: no Fail, since
// there is no client session to stop.
constexpr int64_t kMaxHostBootMs = 180'000;
// How recently a beacon must have arrived for a failure to read as "the host is stuck" rather than
// "the host went quiet". Three times the beacon's own second, so one dropped note does not change
// the sentence.
constexpr int64_t kBeaconFreshMs = 3'000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Phase PhaseOf() { return static_cast<Phase>(g_phase.load(std::memory_order_relaxed)); }

// The token advanced (or a phase began, which is its first advance). One store.
void StampToken() { g_tokenMs.store(NowMs(), std::memory_order_relaxed); }

// What each phase waits on, and for how long. A budget of 0 means this phase is not watched from
// here. The numbers are per PHASE, not per join: each is "this step has produced nothing for this
// long", so a slow link and a mature world cost time in the step, never the budget.
struct PhaseWatch {
    int64_t    budgetMs;
    EndReason  code;
    const char* detail;
};

PhaseWatch WatchFor(Phase ph, Stage st) {
    switch (ph) {
    case Phase::Connecting:
        // Only the wait for the host's world: the dial, the route and the identity exchange have
        // their own reasons from the transport, and an in-gameplay join (Stage::Joining) is waiting
        // for a bracket it will ask for at world-ready, not for a world to be prepared.
        if (st == Stage::WaitingForWorld)
            return {30'000, EndReason::HostWorldNotPrepared, "the host never finished preparing its world"};
        return {0, EndReason::None, ""};
    case Phase::Downloading:
        return {20'000, EndReason::WorldDownloadStalled, "no byte of the world arrived"};
    // LoadingWorld spans two things the engine load, which is opaque and blocks the game thread
    // so no watchdog here can tick through it, and the window AFTER it, where the render is alive
    // and this client is waiting on its own announce gates. The budget is on the second: it starts
    // when the pump first names a gate, and only a gate that CHANGED renews it. 150 s clears the
    // quiescence probe's own 120 s ceiling, the one gate that legitimately holds this long.
    case Phase::LoadingWorld:
        return {150'000, EndReason::WorldNeverSettled,
                "this machine never finished settling into the world it loaded"};
    case Phase::AwaitingWorldStream:
    case Phase::Receiving:
        return {30'000, EndReason::HostWorldNotSent, "the host never sent the world it owed"};
    default:
        return {0, EndReason::None, ""};
    }
}

// The host's own phases, for the one line a field log needs: what the other side said it was
// doing. Kept beside the client's phase names so a reader sees both vocabularies at once.
const char* HostPhaseName(uint8_t p) {
    switch (static_cast<coop::net::HostJoinPhase>(p)) {
    case coop::net::HostJoinPhase::CapturingWorld:    return "capturing its world";
    case coop::net::HostJoinPhase::StreamingWorld:    return "streaming its world";
    case coop::net::HostJoinPhase::SnapshotDeferred:  return "holding the bracket until its world settles";
    case coop::net::HostJoinPhase::StreamingSnapshot: return "streaming the bracket";
    }
    return "in a phase this build does not know";
}

const char* WorldGateName(uint8_t g) {
    switch (static_cast<WorldGate>(g)) {
    case WorldGate::NoLocalPlayer:      return "no local player has appeared in the loaded world";
    case WorldGate::RegistryUnseeded:   return "the prop registry has not seeded once";
    case WorldGate::RegistryOtherWorld: return "the prop registry expresses a different world";
    case WorldGate::RegistryPurging:    return "the prop registry is still draining a dead world";
    case WorldGate::WorldSettling:      return "the load tail has not settled";
    case WorldGate::None:               return "nothing -- every gate is clear";
    default:                            return "a gate this build does not name";
    }
}

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Connecting:          return "Connecting";
    case Phase::Downloading:         return "Downloading";
    case Phase::LoadingWorld:        return "LoadingWorld";
    case Phase::AwaitingWorldStream: return "AwaitingWorldStream";
    case Phase::Receiving:           return "Receiving";
    default:                         return "Idle";
    }
}

namespace {

const char* StageName(Stage s) {
    switch (s) {
    case Stage::FindingHost:     return "FindingHost";
    case Stage::Dialing:         return "Dialing";
    case Stage::FindingRoute:    return "FindingRoute";
    case Stage::ProvingIdentity: return "ProvingIdentity";
    case Stage::Joining:         return "Joining";
    case Stage::WaitingForWorld: return "WaitingForWorld";
    default:                     return "None";
    }
}

void ClearNoticeLocked() {
    g_notice = Notice{};
    g_noticePending.store(false);
}

// The one stash: the first notice of an attempt wins when `firstWins` (Fail's rule, since the
// detector that fails a join re-fires every tick); a refusal or a disconnect replaces whatever
// was there, since nothing else is racing to explain the same end.
void Stash(EndReason code, const std::string& detail, bool afterJoin, bool firstWins) {
    std::lock_guard<std::mutex> lk(g_noticeMu);
    if (firstWins && g_notice.code != EndReason::None) return;
    g_notice.code = code;
    g_notice.detail = detail;
    g_notice.afterJoin = afterJoin;
    g_noticePending.store(true);
}

void ResetCounters() {
    g_applied.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_abortReq.store(false, std::memory_order_relaxed);
    g_hostPhase.store(0, std::memory_order_relaxed);
    g_hostDone.store(0, std::memory_order_relaxed);
    g_hostTotal.store(0, std::memory_order_relaxed);
    g_hostBeacons.store(0, std::memory_order_relaxed);
    g_hostBeaconMs.store(0, std::memory_order_relaxed);
    g_worldGate.store(0, std::memory_order_relaxed);
}

}  // namespace

void BeginConnect(const std::string& hostLabel, Stage first) {
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        g_host = hostLabel;
    }
    g_mode.store(static_cast<int>(Mode::Client), std::memory_order_relaxed);
    ResetCounters();
    // A fresh attempt clears any prior notice so a retry starts clean (the dialog otherwise
    // lives until the player acknowledges it).
    { std::lock_guard<std::mutex> lk(g_noticeMu); ClearNoticeLocked(); }
    const int64_t now = NowMs();
    g_startMs.store(now, std::memory_order_relaxed);
    g_stageStartMs.store(now, std::memory_order_relaxed);
    g_tokenMs.store(now, std::memory_order_relaxed);
    g_stage.store(static_cast<int>(first), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Connecting), std::memory_order_release);
    UE_LOGI("join_progress: BeginConnect -- loading screen up (connecting to '%s', stage %s)",
            hostLabel.c_str(), StageName(first));
}

void BeginHostBoot(const std::string& worldLabel) {
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        g_host = worldLabel;
    }
    g_mode.store(static_cast<int>(Mode::Host), std::memory_order_relaxed);
    ResetCounters();
    // Hosting after a failed join: drop any lingering notice.
    { std::lock_guard<std::mutex> lk(g_noticeMu); ClearNoticeLocked(); }
    const int64_t now = NowMs();
    g_startMs.store(now, std::memory_order_relaxed);
    g_stageStartMs.store(now, std::memory_order_relaxed);
    g_tokenMs.store(now, std::memory_order_relaxed);
    g_stage.store(static_cast<int>(Stage::None), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Connecting), std::memory_order_release);
    UE_LOGI("join_progress: BeginHostBoot -- host loading cover up (loading world '%s'); menu hidden",
            worldLabel.c_str());
}

void NoteStage(Stage stage) {
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    if (PhaseOf() != Phase::Connecting) return;
    const int want = static_cast<int>(stage);
    if (g_stage.load(std::memory_order_relaxed) == want) return;
    g_stage.store(want, std::memory_order_relaxed);
    g_stageStartMs.store(NowMs(), std::memory_order_relaxed);
    StampToken();  // a new stage waits on its own token, not on the one before it
    UE_LOGI("join_progress: stage %s", StageName(stage));
}

void BeginSnapshot(uint32_t propTotal) {
    // Browser joins only: the loading screen tracks the snapshot only when a join raised it.
    // Every client receives the host's snapshot-begin marker on connect, including a scripted
    // client already in gameplay that must not have a loading screen and console pop up over it
    // mid-walk, so outside a join (idle) the marker is ignored. Receiving is accepted too: after
    // a mid-drain world-transition abort with no snapshot-complete, the deferred re-bracket's
    // snapshot-begin must refresh the stale denominator or the bar pegs at a wrong total for the
    // full re-stream. Downloading and loading-world are accepted for the same reason as
    // connecting: they are the phases a save-transfer joiner is actually in when the host's
    // replay arrives, and omitting them would leave the prop bar dead for every menu-mode join.
    const Phase ph = PhaseOf();
    if (ph != Phase::Connecting && ph != Phase::Downloading &&
        ph != Phase::LoadingWorld && ph != Phase::AwaitingWorldStream &&
        ph != Phase::Receiving) return;
    g_total.store(propTotal, std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    g_stageStartMs.store(NowMs(), std::memory_order_relaxed);
    StampToken();
    g_phase.store(static_cast<int>(Phase::Receiving), std::memory_order_release);
    UE_LOGI("join_progress: BeginSnapshot -- receiving world (%u objects)", propTotal);
}

void NoteDownload(uint32_t doneBytes, uint32_t totalBytes) {
    // Client joins only. A host boot shares the cover and must never be relabelled as
    // downloading the world: it is loading its own save off disk.
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    if (totalBytes == 0) return;  // Begin has not landed yet -- stay indeterminate
    // Compare-exchange, not a read-then-store. This runs on the timeline thread while the game
    // thread can reset from the pump's aggregate-disconnect edge, and the loop that calls this is
    // itself posting that tick. A blind store on a stale read would re-raise the cover a reset
    // had just taken down (active again, byte counters re-written non-zero), and against a
    // snapshot-begin it would stomp the receiving phase so the prop bar never filled.
    int expected = static_cast<int>(Phase::Connecting);
    if (g_phase.compare_exchange_strong(expected, static_cast<int>(Phase::Downloading),
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        g_stageStartMs.store(NowMs(), std::memory_order_relaxed);
        StampToken();
        UE_LOGI("join_progress: Downloading -- world blob %u bytes", totalBytes);
    } else if (expected != static_cast<int>(Phase::Downloading)) {
        return;  // the phase moved out from under us -- write nothing
    }
    // Only now, with the phase confirmed ours, are the counters ours to write. The token advances
    // on a byte that ARRIVED, never on the poll that reports the same number again: this loop calls
    // in at 60 Hz whether or not the link is carrying anything, so a store per call would make the
    // watchdog measure the poll instead of the transfer.
    if (doneBytes > g_dlDone.load(std::memory_order_relaxed)) StampToken();
    g_dlDone.store(doneBytes > totalBytes ? totalBytes : doneBytes, std::memory_order_relaxed);
    g_dlTotal.store(totalBytes, std::memory_order_relaxed);
}

void BeginWorldLoad() {
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    // The same compare-exchange discipline, accepting either predecessor: a normal join arrives
    // from downloading, while a host with no save, or one that never sent a begin, never left
    // connecting.
    for (const Phase from : {Phase::Downloading, Phase::Connecting}) {
        int expected = static_cast<int>(from);
        if (g_phase.compare_exchange_strong(expected, static_cast<int>(Phase::LoadingWorld),
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            g_stageStartMs.store(NowMs(), std::memory_order_relaxed);
            StampToken();
            // The cover goes up HERE, which is what its own contract says it is for: the client's
            // save load-in, the camera settle, the spawn burst and the reposition jumps the engine
            // makes with its own actors. Raised at the bracket instead, it covered the last second
            // or two of a join and left the raw world on screen for the whole world load and the
            // wait for the host's bracket -- the joiner stood in a half-built world reading a panel
            // that said it was still loading. Idempotent: the bracket's own Show is then a no-op,
            // and an in-gameplay join, which never loads a world, still raises it there.
            coop::join_curtain::Show();
            UE_LOGI("join_progress: LoadingWorld -- blob in, engine loading it (curtain up)");
            return;
        }
    }
}

void NoteWorldReady() {
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    // The same compare-exchange discipline as the other transitions, accepting both predecessors:
    // a menu-mode join arrives from the engine load, an in-gameplay join never left connecting
    // because it downloaded and loaded nothing.
    for (const Phase from : {Phase::LoadingWorld, Phase::Connecting}) {
        int expected = static_cast<int>(from);
        if (g_phase.compare_exchange_strong(expected, static_cast<int>(Phase::AwaitingWorldStream),
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            g_stageStartMs.store(NowMs(), std::memory_order_relaxed);
            StampToken();
            UE_LOGI("join_progress: AwaitingWorldStream -- world up and announced; the host owes the bracket");
            return;
        }
    }
}

void NoteWorldReadyGate(WorldGate gate) {
    if (!Active()) return;
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    const uint8_t g = static_cast<uint8_t>(gate);
    if (g_worldGate.exchange(g, std::memory_order_relaxed) == g) return;  // liveness, not progress
    // A CHANGED gate is this machine making its way forward, and it re-bases the wait. The first
    // report after the engine load does too, which is what keeps the load's own seconds -- spent
    // with no frames and no ticks -- out of the budget that follows it.
    StampToken();
    UE_LOGI("join_progress: waiting on this machine -- %s", WorldGateName(g));
}

void NoteHostBeacon(uint8_t phase, uint32_t done, uint32_t total) {
    if (!Active()) return;
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    const uint8_t  was = g_hostPhase.exchange(phase, std::memory_order_relaxed);
    const uint32_t wasDone = g_hostDone.exchange(done, std::memory_order_relaxed);
    g_hostTotal.store(total, std::memory_order_relaxed);
    g_hostBeacons.fetch_add(1, std::memory_order_relaxed);
    g_hostBeaconMs.store(NowMs(), std::memory_order_relaxed);
    // One line per CHANGE, never per beacon: four lines in a whole join, and they are the only
    // record anywhere of what the host was doing while this joiner waited.
    if (was != phase)
        UE_LOGI("join_progress: the host is %s (%u/%u)", HostPhaseName(phase), done, total);
    // THE TOKEN IS PROGRESS, NOT ARRIVAL. A beacon repeating the same phase with the same number
    // says the host is alive, which is worth knowing and is not worth waiting on: renewing the
    // wait on it lets a host that is stuck hold a joiner forever, which is the failure the whole
    // watchdog exists to end (measured: a host holding its bracket kept a joiner waiting out 125
    // beacons and 120 seconds). A phase that changed, or a numerator that moved, IS progress.
    if (was != phase || done > wasDone) StampToken();
}

void NotePropApplied() {
    if (PhaseOf() != Phase::Receiving) return;  // free outside a join
    const uint32_t total = g_total.load(std::memory_order_relaxed);
    StampToken();  // a prop applied IS the bracket's progress token
    const uint32_t cur = g_applied.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cur > total) g_applied.store(total, std::memory_order_relaxed);  // clamp (live spawns during the window)
}

void Complete() {
    if (PhaseOf() == Phase::Idle) return;
    const uint32_t total = g_total.load(std::memory_order_relaxed);
    const uint32_t applied = g_applied.load(std::memory_order_relaxed);
    g_applied.store(total, std::memory_order_relaxed);  // snap to 100% for any in-flight read
    // Symmetric with Reset, which clears these too. Harmless today (the idle early return in the
    // loading screen gates every read), but an asymmetry here is exactly what hands the next
    // cover a stale denominator.
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_stage.store(static_cast<int>(Stage::None), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
    UE_LOGI("join_progress: Complete -- loading screen down (applied %u/%u, %u host beacons heard)",
            applied, total, g_hostBeacons.load(std::memory_order_relaxed));
    coop::rig_ready::Say("joined");
}

void Reset() {
    if (g_phase.exchange(static_cast<int>(Phase::Idle), std::memory_order_release) ==
        static_cast<int>(Phase::Idle)) {
        return;  // already hidden -- no log spam
    }
    ResetCounters();
    g_stage.store(static_cast<int>(Stage::None), std::memory_order_relaxed);
    // An abort from a non-idle phase (cancel, connect failure, failsafe) reached here: the normal
    // snapshot-complete path dismisses the curtain from the feed and calls Complete, never Reset
    // from a non-idle phase. Drop the cover so it cannot trap the menu black.
    coop::join_curtain::Reset();
    UE_LOGI("join_progress: Reset -- loading screen hidden");
}

void RequestCancel() {
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting -- we did NOT win
    // We won the abort as a player cancel: silent, no failure modal. Clear any notice a losing
    // Fail set in a race (the winner defines the abort's semantics).
    { std::lock_guard<std::mutex> lk(g_noticeMu); ClearNoticeLocked(); }
    UE_LOGI("join_progress: Cancel requested -- aborting the join");
}

void Fail(EndReason code, const std::string& detail) {
    // No-op unless a browser join is in flight (so a host or scripted-boot failure cannot pop a
    // client cover) and idempotent (the connect-fail detector re-fires every tick until the
    // harness drains the abort; log and flag exactly once).
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting -- we did NOT win
    // We won the abort as a failure: stash the notice for the end-reason modal, unless the
    // process is tearing down (no UI to show; a shutting-down reason must not pop a modal). Set
    // only by the winner, so a racing cancel that won first keeps it silent. And the first notice
    // of an attempt wins: the abort flag is drained by the harness the moment it acts on the
    // abort, so a detector that re-fires (the connect-fail edge re-fires every tick by design)
    // wins the exchange a second time and used to overwrite the stashed reason with the generic
    // fallback, because the specific reason had already been moved out of the session by the
    // first take of the host's close reason. Safe within an attempt and across them:
    // BeginConnect, BeginHostBoot and Cancel each clear the notice, so this keeps the cause of
    // one failed join and never leaks it into the next.
    bool kept = false;
    if (!coop::shutdown::IsShuttingDown()) {
        std::lock_guard<std::mutex> lk(g_noticeMu);
        if (g_notice.code == EndReason::None) {
            g_notice.code = code;
            g_notice.detail = detail;
            g_notice.afterJoin = false;
            g_noticePending.store(true);
            kept = true;
        }
    }
    const coop::net::EndReasonInfo& info = Describe(code);
    UE_LOGW("join_progress: join FAILED [%s] %s%s%s -- aborting + reopening the browser%s",
            info.id, info.text, detail.empty() ? "" : " -- ", detail.c_str(),
            kept ? "" : " [notice NOT shown -- an earlier one stands]");
}

void RefuseJoin(EndReason code, const std::string& detail) {
    // A pre-flight rejection (the version gate): nothing is in flight, no cover was raised and
    // there is no abort to request, so unlike Fail there is no active gate. Just stash the
    // notice; the end-reason dialog renders on the pending flag alone, and its OK button or the
    // next BeginConnect clears it.
    if (coop::shutdown::IsShuttingDown()) return;
    Stash(code, detail, /*afterJoin*/false, /*firstWins*/false);
    const coop::net::EndReasonInfo& info = Describe(code);
    UE_LOGW("join_progress: join REFUSED pre-flight [%s] %s%s%s", info.id, info.text,
            detail.empty() ? "" : " -- ", detail.c_str());
}

void NoteDisconnect(EndReason code, const std::string& detail, bool afterJoin) {
    if (coop::shutdown::IsShuttingDown()) return;
    Stash(code, detail, afterJoin, /*firstWins*/!afterJoin);
    const coop::net::EndReasonInfo& info = Describe(code);
    UE_LOGW("join_progress: %s [%s] %s%s%s",
            afterJoin ? "DISCONNECTED" : "join FAILED (the link ended mid-join)", info.id,
            info.text, detail.empty() ? "" : " -- ", detail.c_str());
}

bool TakeAbortRequest() { return g_abortReq.exchange(false, std::memory_order_acq_rel); }

bool PeekNotice(Notice& out) {
    std::lock_guard<std::mutex> lk(g_noticeMu);
    if (g_notice.code == EndReason::None) return false;
    out = g_notice;
    return true;
}

void ClearNotice() {
    std::lock_guard<std::mutex> lk(g_noticeMu);
    ClearNoticeLocked();
}

bool NoticePending() { return g_noticePending.load(std::memory_order_relaxed); }

bool Active() { return PhaseOf() != Phase::Idle; }

Phase CurrentPhase() { return PhaseOf(); }

View Snapshot() {
    View v;
    v.phase = PhaseOf();
    v.stage = static_cast<Stage>(g_stage.load(std::memory_order_relaxed));
    v.mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    v.applied = g_applied.load(std::memory_order_relaxed);
    v.total = g_total.load(std::memory_order_relaxed);
    v.doneBytes = g_dlDone.load(std::memory_order_relaxed);
    v.totalBytes = g_dlTotal.load(std::memory_order_relaxed);
    const int64_t stageStart = g_stageStartMs.load(std::memory_order_relaxed);
    v.stageMs = (stageStart == 0) ? 0 : static_cast<uint64_t>(NowMs() - stageStart);
    v.hostPhase = g_hostPhase.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        v.host = g_host;
    }
    return v;
}

void MaybeTimeout() {
    if (!Active()) return;
    // A host boot: the harness owns the lifecycle (a reset on session start or its own timeout).
    // This is only a last-resort backstop so a wedged host boot cannot trap the cover forever:
    // just drop it (no Fail, there is no client session to stop). g_startMs is read HERE and
    // nowhere else now -- the client's waits are measured from their own phase's token, so a
    // whole-attempt stopwatch would be a number no client-side rule consults.
    if (static_cast<Mode>(g_mode.load(std::memory_order_relaxed)) == Mode::Host) {
        const int64_t start = g_startMs.load(std::memory_order_relaxed);
        if (start != 0 && NowMs() - start > kMaxHostBootMs) {
            UE_LOGW("join_progress: host-boot cover exceeded %llds -- dropping it (failsafe)",
                    static_cast<long long>(kMaxHostBootMs / 1000));
            Reset();
        }
        return;
    }
    // THE PHASE WATCHDOG IS THE ONLY CLIENT-SIDE JOIN WATCHDOG. There is no whole-join failsafe
    // behind it any more: a join ends when a phase's token stops or when the transport dies, never
    // because the sum of legitimate phase durations crossed a number. The one it replaced spanned
    // the dial, the host's live capture, the download, the engine world load and the bracket --
    // phases whose honest durations are independent and additive -- so all it could report was
    // that the sum ran long, in a sentence that guessed at its own cause. Fail is idempotent and
    // first-notice-wins, so whichever phase fires owns the dialog.
    const Phase ph = PhaseOf();
    const PhaseWatch w = WatchFor(ph, static_cast<Stage>(g_stage.load(std::memory_order_relaxed)));
    const int64_t token = g_tokenMs.load(std::memory_order_relaxed);
    if (w.budgetMs > 0 && token != 0 && NowMs() - token > w.budgetMs) {
        // The two ways a phase dies read differently in a report, so they read differently here:
        // a host that went quiet, and a host that answered every second and got nowhere. Both name
        // the phase, which is the whole difference from the guess this replaces.
        const int64_t now = NowMs();
        const long long stuckS = static_cast<long long>((now - token) / 1000);
        const int64_t beaconMs = g_hostBeaconMs.load(std::memory_order_relaxed);
        const uint8_t hp = g_hostPhase.load(std::memory_order_relaxed);
        char detail[224];
        // LoadingWorld is the one phase whose blocker is OURS, so it names our own gate; the host
        // is not at fault and the sentence must not point at it.
        if (ph == Phase::LoadingWorld) {
            std::snprintf(detail, sizeof(detail),
                          "%s: %s, and nothing has changed in %llds", PhaseName(ph),
                          WorldGateName(g_worldGate.load(std::memory_order_relaxed)), stuckS);
        } else if (hp != 0 && beaconMs != 0 && now - beaconMs <= kBeaconFreshMs) {
            std::snprintf(detail, sizeof(detail),
                          "%s: the host says it is %s and nothing has moved in %llds (%u/%u)",
                          PhaseName(ph), HostPhaseName(hp), stuckS,
                          g_hostDone.load(std::memory_order_relaxed),
                          g_hostTotal.load(std::memory_order_relaxed));
        } else if (hp != 0) {
            std::snprintf(detail, sizeof(detail),
                          "%s: the host was %s and stopped answering %llds ago",
                          PhaseName(ph), HostPhaseName(hp),
                          static_cast<long long>((now - beaconMs) / 1000));
        } else {
            std::snprintf(detail, sizeof(detail), "%s: %s, and the host never answered at all",
                          PhaseName(ph), w.detail);
        }
        Fail(w.code, detail);
        return;
    }
}

}  // namespace coop::join_progress
