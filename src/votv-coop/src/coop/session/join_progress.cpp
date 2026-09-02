// coop/join_progress.cpp -- see coop/join_progress.h.

#include "coop/session/join_progress.h"

#include "ui/join_curtain.h"  // instant-world: drop the curtain on a join ABORT (not the normal Complete path)
#include "coop/session/shutdown.h"  // IsShuttingDown -- suppress the failure dialog during teardown
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace coop::join_progress {
namespace {

// phase + counts are atomics (written on the net-drain thread, read on the render
// thread); the host label is a std::string under its own mutex (written rarely in
// BeginConnect, read once/frame in Snapshot).
std::atomic<int>      g_phase{static_cast<int>(Phase::Idle)};
std::atomic<int>      g_mode{static_cast<int>(Mode::Client)};
std::atomic<uint32_t> g_applied{0};
std::atomic<uint32_t> g_total{0};
// World-blob download counters (bytes). Written by the harness join loop via
// NoteDownload, read on the render thread in Snapshot.
std::atomic<uint32_t> g_dlDone{0};
std::atomic<uint32_t> g_dlTotal{0};
std::atomic<int64_t>  g_startMs{0};
std::atomic<bool>     g_abortReq{false};  // Cancel button OR a connect failure -> harness drains (Stop + reopen browser)

std::mutex  g_hostMu;
std::string g_host;  // guarded by g_hostMu

// Connect-failure reason for ui/connect_failed_dialog (see the header). Set by the
// abort WINNER (Fail stashes / Cancel clears) and read/cleared by the render thread;
// its own mutex, independent of g_abortReq (which the harness drains separately).
std::mutex  g_failMu;
std::string g_failReason;  // guarded by g_failMu; non-empty == a modal is pending
// Lock-free mirror of "g_failReason is non-empty" so the per-frame overlay GATE
// (FailPending) never takes g_failMu -- only Render's actual string read does. Kept
// in sync inside g_failMu's critical sections.
std::atomic<bool> g_failPending{false};

// Generous failsafe: v56 save-transfer joins legitimately spend the cover on a
// ~17 MB save download (WAN: tens of seconds) + the full world load (~30-60 s)
// + the true-up bracket. The harness DriveMenuModeJoinWorldBoot owns the real
// 120 s transfer timeout; this longer cap only fires if that path wedged (a
// trapped cover is worse than revealing the game).
constexpr int64_t kMaxJoinMs = 240'000;
// Host-boot failsafe backstop. The harness DriveHostBootIfPending normally Reset()s
// this on session-start / its own ~120 s timeout; this longer cap only fires if that
// path somehow never ran (a stuck cover at the menu is worse than dropping it). Host
// mode just Resets (no Fail -- there is no client session to Stop).
constexpr int64_t kMaxHostBootMs = 180'000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Phase PhaseOf() { return static_cast<Phase>(g_phase.load(std::memory_order_relaxed)); }

}  // namespace

void BeginConnect(const std::string& hostLabel) {
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        g_host = hostLabel;
    }
    g_mode.store(static_cast<int>(Mode::Client), std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_abortReq.store(false, std::memory_order_relaxed);
    // A fresh attempt clears any prior failure modal so a retry starts clean (the
    // dialog otherwise lives until the user acknowledges it).
    { std::lock_guard<std::mutex> lk(g_failMu); g_failReason.clear(); g_failPending.store(false); }
    g_startMs.store(NowMs(), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Connecting), std::memory_order_release);
    UE_LOGI("join_progress: BeginConnect -- loading screen up (connecting to '%s')",
            hostLabel.c_str());
}

void BeginHostBoot(const std::string& worldLabel) {
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        g_host = worldLabel;
    }
    g_mode.store(static_cast<int>(Mode::Host), std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_abortReq.store(false, std::memory_order_relaxed);
    // Hosting after a failed join -- drop any lingering connect-failure modal.
    { std::lock_guard<std::mutex> lk(g_failMu); g_failReason.clear(); g_failPending.store(false); }
    g_startMs.store(NowMs(), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Connecting), std::memory_order_release);
    UE_LOGI("join_progress: BeginHostBoot -- host loading cover up (loading world '%s'); menu hidden",
            worldLabel.c_str());
}

void BeginSnapshot(uint32_t propTotal) {
    // BROWSER-JOIN ONLY (regression A completion, 2026-06-06). The loading screen tracks the
    // snapshot ONLY when a browser join raised it (BeginConnect -> Connecting). EVERY client
    // receives the host's v34 SnapshotBegin marker on connect -- including the env/.bat/
    // autotest client, which is ALREADY in gameplay and must NOT have a loading screen +
    // console pop up over it mid-walk (the user-reported bug + a per-frame render cost).
    // So if we are not in a browser join (phase != Connecting), ignore the marker entirely.
    // (The earlier 'adopt it anyway' was the bug: it popped the cover for env clients.)
    // Fork A (2026-06-10): ALSO accept Receiving -- after a mid-drain world-
    // transition abort (no SnapshotComplete), the deferred re-bracket's
    // SnapshotBegin must refresh the stale denominator or the bar pegs at a
    // wrong total for the full ~2300-prop re-stream. Env/autotest clients
    // are Idle and still ignored (the regression-A target).
    // Downloading is accepted for the SAME reason Connecting is: it is the phase a
    // save-transfer joiner is actually in when the host's replay arrives, and it did
    // not exist when this gate was written. Omitting it would have left the prop bar
    // dead for every menu-mode join -- the regression this list's shape invites.
    const Phase ph = PhaseOf();
    if (ph != Phase::Connecting && ph != Phase::Downloading &&
        ph != Phase::LoadingWorld && ph != Phase::Receiving) return;
    g_total.store(propTotal, std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    if (g_startMs.load(std::memory_order_relaxed) == 0) g_startMs.store(NowMs(), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Receiving), std::memory_order_release);
    UE_LOGI("join_progress: BeginSnapshot -- receiving world (%u objects)", propTotal);
}

void NoteDownload(uint32_t doneBytes, uint32_t totalBytes) {
    // Client joins only. A HOST boot shares the cover (Mode::Host) and must never be
    // relabelled "Downloading the world" -- it is loading its OWN save off disk.
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    if (totalBytes == 0) return;  // Begin has not landed yet -- stay indeterminate
    // COMPARE-EXCHANGE, NOT A READ-THEN-STORE. This runs on the TIMELINE thread while
    // the game thread can call Reset() from net_pump's aggregate-disconnect edge --
    // and the loop that calls this is itself posting that tick. A blind store on a
    // stale read would re-raise the cover a Reset had just taken down (Active() true
    // again, byte counters re-written non-zero), and against BeginSnapshot it would
    // stomp Receiving so the prop bar never filled. Post-ship audit, 2026-09-02.
    int expected = static_cast<int>(Phase::Connecting);
    if (g_phase.compare_exchange_strong(expected, static_cast<int>(Phase::Downloading),
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        UE_LOGI("join_progress: Downloading -- world blob %u bytes", totalBytes);
    } else if (expected != static_cast<int>(Phase::Downloading)) {
        return;  // the phase moved out from under us -- write nothing
    }
    // Only now, with the phase confirmed OURS, are the counters ours to write.
    g_dlDone.store(doneBytes > totalBytes ? totalBytes : doneBytes, std::memory_order_relaxed);
    g_dlTotal.store(totalBytes, std::memory_order_relaxed);
}

void BeginWorldLoad() {
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::Client)) return;
    // Same CAS discipline, and it accepts EITHER predecessor: a normal join arrives
    // from Downloading, while a no-save / never-sent-Begin host never left Connecting.
    for (const Phase from : {Phase::Downloading, Phase::Connecting}) {
        int expected = static_cast<int>(from);
        if (g_phase.compare_exchange_strong(expected, static_cast<int>(Phase::LoadingWorld),
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            UE_LOGI("join_progress: LoadingWorld -- blob in, engine loading it");
            return;
        }
    }
}

void NotePropApplied() {
    if (PhaseOf() != Phase::Receiving) return;  // free outside a join
    const uint32_t total = g_total.load(std::memory_order_relaxed);
    const uint32_t cur = g_applied.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cur > total) g_applied.store(total, std::memory_order_relaxed);  // clamp (live spawns during the window)
}

void Complete() {
    if (PhaseOf() == Phase::Idle) return;
    const uint32_t total = g_total.load(std::memory_order_relaxed);
    const uint32_t applied = g_applied.load(std::memory_order_relaxed);
    g_applied.store(total, std::memory_order_relaxed);  // snap to 100% for any in-flight read
    // Symmetric with Reset(), which clears these too. Harmless today (the Idle
    // early-return in loading_screen gates every read), but an asymmetry here is
    // exactly what hands the NEXT cover a stale denominator.
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Idle), std::memory_order_release);
    UE_LOGI("join_progress: Complete -- loading screen down (applied %u/%u)", applied, total);
}

void Reset() {
    if (g_phase.exchange(static_cast<int>(Phase::Idle), std::memory_order_release) ==
        static_cast<int>(Phase::Idle)) {
        return;  // already hidden -- no log spam
    }
    g_applied.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_dlDone.store(0, std::memory_order_relaxed);
    g_dlTotal.store(0, std::memory_order_relaxed);
    g_abortReq.store(false, std::memory_order_relaxed);
    // instant-world: an ABORT from a non-Idle phase (cancel / connect-fail / failsafe) reached here (the
    // normal SnapshotComplete path uses Complete(), which fades the curtain via BeginDismiss and never calls
    // Reset from non-Idle). Drop the cover so it can't trap the menu black. (Idle-already early-returned above.)
    coop::join_curtain::Reset();
    UE_LOGI("join_progress: Reset -- loading screen hidden");
}

void RequestCancel() {
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting -- we did NOT win
    // We won the abort as a user CANCEL: silent, no failure modal. Clear any reason a
    // losing Fail set in a race (the winner defines the abort's semantics).
    { std::lock_guard<std::mutex> lk(g_failMu); g_failReason.clear(); g_failPending.store(false); }
    UE_LOGI("join_progress: Cancel requested -- aborting the join");
}

void Fail(const std::string& reason) {
    // No-op unless a browser join is in flight (so a host / env-boot failure can't pop a
    // client cover) and idempotent (the connect-fail detector re-fires every tick until the
    // harness drains the abort -- log + flag exactly once).
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting -- we did NOT win
    // We won the abort as a FAILURE: stash the reason for ui/connect_failed_dialog, unless
    // the process is tearing down (no UI to show; a "shutting down" reason must not pop a
    // modal). Set only by the winner, so a racing Cancel that won first keeps it silent.
    //
    // ...AND THE FIRST REASON OF AN ATTEMPT WINS, which is the half of "idempotent"
    // this function only claimed to have. `g_abortReq` is DRAINED by the harness
    // (TakeAbortRequest) the moment it acts on the abort, so a detector that re-fires
    // -- and the connect-fail edge re-fires every tick by design -- wins the exchange
    // a second time and used to overwrite the stashed reason. Measured 2026-09-01: a
    // client refused a locked host, stashed the sentence saying exactly why, and two
    // ticks later replaced it with the generic "could not connect to the host",
    // because the specific reason had already been MOVED out of the session by the
    // first TakeHostCloseReason. The player read the fallback; the real reason
    // existed, was correct, and was destroyed by its own success.
    //
    // Safe within an attempt and across them: BeginConnect / BeginHostBoot / Cancel
    // each clear g_failReason, so this keeps the CAUSE of one failed join and never
    // leaks it into the next.
    bool kept = false;
    if (!coop::shutdown::IsShuttingDown()) {
        std::lock_guard<std::mutex> lk(g_failMu);
        if (g_failReason.empty()) {
            g_failReason = reason;
            g_failPending.store(true);
            kept = true;
        }
    }
    UE_LOGW("join_progress: join FAILED (%s) -- aborting + reopening the browser%s",
            reason.c_str(), kept ? "" : " [reason NOT shown -- an earlier one stands]");
}

void RefuseJoin(const std::string& reason) {
    // Pre-flight rejection (v122 version gate): nothing is in flight -- no cover was
    // raised, no abort to request -- so unlike Fail there is NO Active() gate. Just
    // stash the reason; connect_failed_dialog renders on FailPending() alone and the
    // OK button / the next BeginConnect clears it (the measured bounded lifecycle).
    if (coop::shutdown::IsShuttingDown()) return;
    {
        std::lock_guard<std::mutex> lk(g_failMu);
        g_failReason = reason;
        g_failPending.store(true);
    }
    UE_LOGW("join_progress: join REFUSED pre-flight (%s)", reason.c_str());
}

bool TakeAbortRequest() { return g_abortReq.exchange(false, std::memory_order_acq_rel); }

bool PeekFailReason(std::string& out) {
    std::lock_guard<std::mutex> lk(g_failMu);
    if (g_failReason.empty()) return false;
    out = g_failReason;
    return true;
}

void ClearFailReason() {
    std::lock_guard<std::mutex> lk(g_failMu);
    g_failReason.clear();
    g_failPending.store(false);
}

bool FailPending() { return g_failPending.load(std::memory_order_relaxed); }

bool Active() { return PhaseOf() != Phase::Idle; }

View Snapshot() {
    View v;
    v.phase = PhaseOf();
    v.mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    v.applied = g_applied.load(std::memory_order_relaxed);
    v.total = g_total.load(std::memory_order_relaxed);
    v.doneBytes = g_dlDone.load(std::memory_order_relaxed);
    v.totalBytes = g_dlTotal.load(std::memory_order_relaxed);
    const int64_t start = g_startMs.load(std::memory_order_relaxed);
    v.elapsedMs = (start == 0) ? 0 : static_cast<uint64_t>(NowMs() - start);
    {
        std::lock_guard<std::mutex> lk(g_hostMu);
        v.host = g_host;
    }
    return v;
}

void MaybeTimeout() {
    if (!Active()) return;
    const int64_t start = g_startMs.load(std::memory_order_relaxed);
    if (start == 0) return;
    // HOST boot: the harness owns the lifecycle (Reset on session-start / its own
    // timeout). This is only a last-resort backstop so a wedged host boot can't trap
    // the cover forever -- just drop it (no Fail: there is no client session to Stop).
    if (static_cast<Mode>(g_mode.load(std::memory_order_relaxed)) == Mode::Host) {
        if (NowMs() - start > kMaxHostBootMs) {
            UE_LOGW("join_progress: host-boot cover exceeded %llds -- dropping it (failsafe)",
                    static_cast<long long>(kMaxHostBootMs / 1000));
            Reset();
        }
        return;
    }
    if (NowMs() - start > kMaxJoinMs) {
        // FAIL, not a bare Reset: Reset hides the cover but never tells the harness to Stop
        // the session, so a stuck/zombie net session keeps net_pump pumping the full gameplay
        // tick at the menu -- the RAM balloon. Fail sets the abort the harness drains (Stop +
        // reopen browser), which actually ends the pump. (audit Issue 7, 2026-06-06.)
        Fail("connection timed out (lost SnapshotComplete or a stalled drain?)");
    }
}

}  // namespace coop::join_progress
