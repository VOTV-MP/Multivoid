// coop/time_sync.cpp -- see coop/time_sync.h. Host-authoritative world-clock sync.

#include "coop/world/time_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/daynightcycle.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>

namespace coop::time_sync {
namespace {

namespace DNC = ue_wrap::daynightcycle;

std::atomic<coop::net::Session*> g_session{nullptr};
std::chrono::steady_clock::time_point g_lastBroadcast{};
bool g_suppressedClient = false;  // U6: we zeroed the local TimeScale (client)
// v71 sleep gate: while the accelerate phase runs, the CLIENT clock free-runs
// at TimeScale=1 (its world is already dilated 20x, so 1.0 advances the clock
// at the host's 20x rate) instead of the U6 TimeScale=0 -- otherwise the sky
// only moves on the 2 s corrections and the timelapse pans in visible 40-game-
// second steps. Corrections keep landing throughout (drift stays bounded);
// coop/sleep_sync toggles this at the phase edges.
std::atomic<bool> g_sleepAccelerate{false};

float ClientTimeScale() { return g_sleepAccelerate.load(std::memory_order_acquire) ? 1.0f : 0.0f; }

// The clock is continuous + the client free-runs its own ReceiveTick at the synced TimeScale,
// so the push is only an anti-drift correction -- a slow rate is plenty and keeps the wire quiet.
constexpr auto kPushInterval = std::chrono::milliseconds(2000);

bool MakePayload(coop::net::TimeSyncPayload& out) {
    float t = 0, d = 0, s = 0;
    if (!DNC::ReadClock(t, d, s)) return false;  // cycle not streamed in yet
    out.totalTime = t; out.day = d; out.timeScale = s;
    // v96: the NAMED clock rides along -- a TimeScale=0 client never runs its own
    // minute pulse, so its HUD clock + day number only move via these corrections.
    int32_t h = 0, m = 0, dz = 0;
    if (!DNC::ReadTimeZ(h, m, dz)) return false;
    out.hour = h; out.minute = m; out.dayZ = dz;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    DNC::EnsureResolved();  // retried via Tick until the cycle class loads
}

void OnReliable(const coop::net::TimeSyncPayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // host is authoritative -- never apply a received clock
    // U6 (v65): the CLIENT runs at TimeScale=0 -- its `day` advances ONLY via
    // these corrections (always < MaxTime: the host wraps in-tick), which
    // makes the midnight task/email/points cascade structurally unreachable
    // client-side (the host's rolls mirror via the email/order channels).
    // The dailyDelivery latch kills the 6am duplicate auto-order, including
    // the join-edge instant fire (the first snap crossing 06:00); it re-
    // asserts with every correction (the suppressed cascade can't reset it,
    // but a fresh save-load mid-session could). Restore on disconnect writes
    // the game's own value (1.0) back.
    DNC::ApplyClock(payload.totalTime, payload.day, ClientTimeScale());
    // v96: mirror the host's NAMED clock (HUD time + the day number in timeZ.Z). The
    // suppressed client minute pulse never rebuilds it locally, and instant host
    // set-clock jumps (dev menu) must land here on the same correction.
    DNC::WriteTimeZ(payload.hour, payload.minute, payload.dayZ);
    DNC::LatchDailyDelivery();
    g_suppressedClient = true;
    static int s_n = 0;
    if ((s_n++ % 5) == 0)  // ~every 10s -- enough to confirm convergence, not spam
        UE_LOGI("time_sync: applied host clock totalTime=%.1f day=%.1f (client scale=%.0f)",
                payload.totalTime, payload.day, ClientTimeScale());
}

void SetSleepAccelerate(bool on) {
    const bool was = g_sleepAccelerate.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;
    auto* s = g_session.load(std::memory_order_acquire);
    // Apply the new scale immediately on the CLIENT (don't wait for the next
    // 2 s correction -- the phase edges should feel instant).
    if (s && s->role() == coop::net::Role::Client && g_suppressedClient)
        DNC::WriteTimeScale(on ? 1.0f : 0.0f);
    UE_LOGI("time_sync: sleep-accelerate %s (client TimeScale -> %.0f)",
            on ? "ON" : "OFF", on ? 1.0f : 0.0f);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    coop::net::TimeSyncPayload p{};
    if (!MakePayload(p)) return;
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::TimeSync, &p, sizeof(p));
    UE_LOGI("time_sync: connect-snapshot -- sent clock (totalTime=%.1f day=%.1f scale=%.3f) to slot %d",
            p.totalTime, p.day, p.timeScale, peerSlot);
}

void Tick() {
    if (!DNC::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;  // host broadcasts; client applies on receipt
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastBroadcast < kPushInterval) return;
    coop::net::TimeSyncPayload p{};
    if (!MakePayload(p)) return;
    g_lastBroadcast = now;
    if (s->SendReliable(coop::net::ReliableKind::TimeSync, &p, sizeof(p))) {
        static int s_n = 0;
        if ((s_n++ % 5) == 0)  // ~every 10s
            UE_LOGI("time_sync: host clock totalTime=%.1f day=%.1f scale=%.3f", p.totalTime, p.day, p.timeScale);
    } else {
        UE_LOGW("time_sync: SendReliable(TimeSync) failed");
    }
}

void OnDisconnect() {
    g_lastBroadcast = {};
    g_sleepAccelerate.store(false, std::memory_order_release);
    if (g_suppressedClient) {
        // U6 restore: 1.0 is the game's own TimeScale restore value (uber
        // @10703). SP day-rolling resumes from the last synced clock; the
        // dailyDelivery latch self-heals at the next local midnight.
        g_suppressedClient = false;
        DNC::WriteTimeScale(1.0f);
        UE_LOGI("time_sync: U6 restore -- client TimeScale back to 1.0");
    }
}

}  // namespace coop::time_sync
