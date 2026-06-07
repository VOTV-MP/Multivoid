// coop/join_progress.cpp -- see coop/join_progress.h.

#include "coop/join_progress.h"

#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace coop::join_progress {
namespace {

// phase + counts are atomics (written on the net-drain thread, read on the render
// thread); the host label is a std::string under its own mutex (written rarely in
// BeginConnect, read once/frame in Snapshot).
std::atomic<int>      g_phase{static_cast<int>(Phase::Idle)};
std::atomic<uint32_t> g_applied{0};
std::atomic<uint32_t> g_total{0};
std::atomic<int64_t>  g_startMs{0};
std::atomic<bool>     g_abortReq{false};  // Cancel button OR a connect failure -> harness drains (Stop + reopen browser)

std::mutex  g_hostMu;
std::string g_host;  // guarded by g_hostMu

// Generous failsafe: a real join (handshake + ~2300 props @ 100/tick) finishes in a
// few seconds even over a TURN relay. 90 s means something is genuinely wrong (lost
// SnapshotComplete, dead drain) -- reveal the game rather than trap the player.
constexpr int64_t kMaxJoinMs = 90'000;

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
    g_applied.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_abortReq.store(false, std::memory_order_relaxed);
    g_startMs.store(NowMs(), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Connecting), std::memory_order_release);
    UE_LOGI("join_progress: BeginConnect -- loading screen up (connecting to '%s')",
            hostLabel.c_str());
}

void BeginSnapshot(uint32_t propTotal) {
    // BROWSER-JOIN ONLY (regression A completion, 2026-06-06). The loading screen tracks the
    // snapshot ONLY when a browser join raised it (BeginConnect -> Connecting). EVERY client
    // receives the host's v34 SnapshotBegin marker on connect -- including the env/.bat/
    // autotest client, which is ALREADY in gameplay and must NOT have a loading screen +
    // console pop up over it mid-walk (the user-reported bug + a per-frame render cost).
    // So if we are not in a browser join (phase != Connecting), ignore the marker entirely.
    // (The earlier 'adopt it anyway' was the bug: it popped the cover for env clients.)
    if (PhaseOf() != Phase::Connecting) return;
    g_total.store(propTotal, std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    if (g_startMs.load(std::memory_order_relaxed) == 0) g_startMs.store(NowMs(), std::memory_order_relaxed);
    g_phase.store(static_cast<int>(Phase::Receiving), std::memory_order_release);
    UE_LOGI("join_progress: BeginSnapshot -- receiving world (%u objects)", propTotal);
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
    g_abortReq.store(false, std::memory_order_relaxed);
    UE_LOGI("join_progress: Reset -- loading screen hidden");
}

void RequestCancel() {
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting
    UE_LOGI("join_progress: Cancel requested -- aborting the join");
}

void Fail(const std::string& reason) {
    // No-op unless a browser join is in flight (so a host / env-boot failure can't pop a
    // client cover) and idempotent (the connect-fail detector re-fires every tick until the
    // harness drains the abort -- log + flag exactly once).
    if (!Active()) return;
    if (g_abortReq.exchange(true, std::memory_order_acq_rel)) return;  // already aborting
    UE_LOGW("join_progress: join FAILED (%s) -- aborting + reopening the browser", reason.c_str());
}

bool TakeAbortRequest() { return g_abortReq.exchange(false, std::memory_order_acq_rel); }

bool Active() { return PhaseOf() != Phase::Idle; }

View Snapshot() {
    View v;
    v.phase = PhaseOf();
    v.applied = g_applied.load(std::memory_order_relaxed);
    v.total = g_total.load(std::memory_order_relaxed);
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
    if (NowMs() - start > kMaxJoinMs) {
        // FAIL, not a bare Reset: Reset hides the cover but never tells the harness to Stop
        // the session, so a stuck/zombie net session keeps net_pump pumping the full gameplay
        // tick at the menu -- the RAM balloon. Fail sets the abort the harness drains (Stop +
        // reopen browser), which actually ends the pump. (audit Issue 7, 2026-06-06.)
        Fail("connection timed out (lost SnapshotComplete or a stalled drain?)");
    }
}

}  // namespace coop::join_progress
