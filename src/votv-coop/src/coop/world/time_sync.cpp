// coop/time_sync.cpp -- see coop/time_sync.h. Host-authoritative world-clock sync.

#include "coop/world/time_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>

namespace coop::time_sync {
namespace {

namespace DNC = ue_wrap::daynightcycle;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_suppressedClient = false;  // U6: we zeroed the local TimeScale (client)
// v71 sleep gate: while the accelerate phase runs, the CLIENT clock free-runs
// at TimeScale=1 (its world is already dilated 20x, so 1.0 advances the clock
// at the host's 20x rate) instead of the U6 TimeScale=0 -- otherwise the sky
// only moves on the 2 s corrections and the timelapse pans in visible 40-game-
// second steps. Corrections keep landing throughout (drift stays bounded);
// coop/sleep_sync toggles this at the phase edges.
std::atomic<bool> g_sleepAccelerate{false};

float ClientTimeScale() { return g_sleepAccelerate.load(std::memory_order_acquire) ? 1.0f : 0.0f; }

// v109 (design F): the client daynightCycle is a PURE host-authoritative mirror frozen at
// TimeScale=0 (never free-runs -- that was the pre-F belief, and the load-bearing invariant is
// "client totalTime never wraps MaxTime locally" -> the midnight cascade stays unreachable). The
// host's clock now streams as an UNRELIABLE ClockPose snapshot at ~500 ms (session net thread), so
// the frozen mirror is refreshed at HH:MM display granularity. The RELIABLE TimeSync(29) is kept
// ONLY for the connect-edge guaranteed initial sync (no periodic reliable push -- RULE 2).

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

// The shared CLIENT-apply: write the host's clock into the local (frozen) daynightCycle. Called
// by BOTH the reliable connect-edge (OnReliable) and the unreliable steady-state stream (Tick's
// client branch). TimeScale is forced to ClientTimeScale() (0 normally, 1 during sleep-accelerate)
// -- NEVER payload.timeScale -- so the client stays a frozen pure mirror and the midnight cascade
// stays structurally unreachable (design F invariant).
void ApplyClockSnapshot(const coop::net::TimeSyncPayload& p) {
    DNC::ApplyClock(p.totalTime, p.day, ClientTimeScale());
    // v96: mirror the host's NAMED clock (HUD time + the day number in timeZ.Z). The suppressed
    // client minute pulse never rebuilds it locally, and instant host set-clock jumps (dev menu)
    // must land here.
    DNC::WriteTimeZ(p.hour, p.minute, p.dayZ);
    // U6: re-assert the daily-delivery latch (the suppressed cascade can't reset it, but a fresh
    // save-load mid-session could).
    DNC::LatchDailyDelivery();
    g_suppressedClient = true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    DNC::EnsureResolved();  // retried via Tick until the cycle class loads
}

void OnReliable(const coop::net::TimeSyncPayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // host is authoritative -- never apply a received clock
    // v109 (design F): the RELIABLE TimeSync is now ONLY the CONNECT-EDGE guaranteed initial sync
    // (QueueConnectBroadcastForSlot). Steady-state corrections ride the unreliable ClockPose stream
    // (Tick's client branch). Both funnel through ApplyClockSnapshot, which keeps the client frozen
    // at TimeScale=0 (U6: `day` advances only via these applies, never wraps MaxTime locally -> the
    // midnight task/email/points cascade stays structurally unreachable; the daily-delivery latch it
    // re-asserts kills the 6am duplicate auto-order). Restore on disconnect writes 1.0 back.
    ApplyClockSnapshot(payload);
    UE_LOGI("time_sync: applied CONNECT-EDGE host clock totalTime=%.1f day=%.1f (client scale=%.0f)",
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
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        // HOST: publish the current clock every tick; the session net thread fan-outs it as an
        // unreliable ClockPose snapshot on its OWN ~500 ms throttle (design F transport). Cheap
        // (two raw reflected reads + a mutex'd copy); MakePayload returns false until the cycle is
        // streamed in, so we never publish garbage.
        coop::net::TimeSyncPayload p{};
        if (MakePayload(p)) s->SetHostClock(true, p);
    } else {
        // CLIENT: drain the latest unreliable host clock + apply on arrival. Between arrivals the
        // frozen mirror holds (pure host-auth mirror -- no local sim). The reliable connect-edge
        // seeds the first value before the stream lands.
        coop::net::TimeSyncPayload p{};
        bool isNew = false;
        if (s->TryGetHostClock(p, &isNew) && isNew) {
            ApplyClockSnapshot(p);
            static int s_n = 0;
            if ((s_n++ % 20) == 0)  // ~every 10s at 2 Hz -- confirm convergence, not spam
                UE_LOGI("time_sync: applied STREAM host clock totalTime=%.1f day=%.1f (client scale=%.0f)",
                        p.totalTime, p.day, ClientTimeScale());
        }
    }
}

void OnDisconnect() {
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
