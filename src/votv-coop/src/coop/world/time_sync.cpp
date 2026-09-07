// coop/world/time_sync.cpp -- see coop/world/time_sync.h. Host-authoritative world-clock sync.

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
bool g_suppressedClient = false;  // we zeroed the local TimeScale (client)
// Sleep gate: while the accelerate phase runs, the CLIENT clock free-runs at TimeScale=1 -- its
// world is already dilated 20x, so 1.0 advances the clock at the host's 20x rate -- instead of the
// usual 0. Otherwise the sky only moves on the corrections and the timelapse pans in visible
// 40-game-second steps. Corrections keep landing throughout, so drift stays bounded;
// coop/sleep_sync toggles this at the phase edges.
std::atomic<bool> g_sleepAccelerate{false};

float ClientTimeScale() { return g_sleepAccelerate.load(std::memory_order_acquire) ? 1.0f : 0.0f; }

// The client daynightCycle is a PURE host-authoritative mirror frozen at TimeScale=0 and never
// free-runs, which is what makes the load-bearing invariant hold: client totalTime never wraps
// MaxTime locally, so the midnight cascade stays unreachable. The host's clock streams as an
// UNRELIABLE ClockPose snapshot about twice a second off the session net thread, refreshing the
// frozen mirror at HH:MM display granularity. The RELIABLE TimeSync is kept only for the
// connect-edge guaranteed initial sync -- there is no periodic reliable push.

bool MakePayload(coop::net::TimeSyncPayload& out) {
    float t = 0, d = 0, s = 0;
    if (!DNC::ReadClock(t, d, s)) return false;  // cycle not streamed in yet
    out.totalTime = t; out.day = d; out.timeScale = s;
    // The NAMED clock rides along: a TimeScale=0 client never runs its own minute pulse, so its HUD
    // clock and day number only move via these corrections.
    int32_t h = 0, m = 0, dz = 0;
    if (!DNC::ReadTimeZ(h, m, dz)) return false;
    out.hour = h; out.minute = m; out.dayZ = dz;
    return true;
}

// The shared CLIENT-apply: write the host's clock into the local, frozen daynightCycle. Called by
// BOTH the reliable connect-edge (OnReliable) and the unreliable steady-state stream (Tick's client
// branch). TimeScale is forced to ClientTimeScale() -- 0 normally, 1 during sleep-accelerate -- and
// NEVER payload.timeScale, so the client stays a frozen pure mirror and the midnight cascade stays
// structurally unreachable.
void ApplyClockSnapshot(const coop::net::TimeSyncPayload& p) {
    DNC::ApplyClock(p.totalTime, p.day, ClientTimeScale());
    // Mirror the host's NAMED clock: the HUD time and the day number in timeZ.Z. The suppressed
    // client minute pulse never rebuilds it locally, and an instant host set-clock jump from the
    // dev menu has to land here.
    DNC::WriteTimeZ(p.hour, p.minute, p.dayZ);
    // Re-assert the daily-delivery latch. The suppressed cascade cannot reset it, but a fresh
    // save-load mid-session could.
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
    // The RELIABLE TimeSync is ONLY the connect-edge guaranteed initial sync
    // (QueueConnectBroadcastForSlot); steady-state corrections ride the unreliable ClockPose stream
    // in Tick's client branch. Both funnel through ApplyClockSnapshot, which keeps the client
    // frozen at TimeScale=0: `day` advances only via these applies and never wraps MaxTime locally,
    // so the midnight task/email/points cascade stays structurally unreachable, and the
    // daily-delivery latch it re-asserts kills the 6am duplicate auto-order. Restore on disconnect
    // writes 1.0 back.
    ApplyClockSnapshot(payload);
    UE_LOGI("time_sync: applied CONNECT-EDGE host clock totalTime=%.1f day=%.1f (client scale=%.0f)",
            payload.totalTime, payload.day, ClientTimeScale());
}

void SetSleepAccelerate(bool on) {
    const bool was = g_sleepAccelerate.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;
    auto* s = g_session.load(std::memory_order_acquire);
    // Apply the new scale immediately on the CLIENT rather than waiting for the next streamed
    // correction -- the phase edges should feel instant.
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
        // HOST: publish the current clock every tick; the session net thread fans it out as an
        // unreliable ClockPose snapshot on its OWN ~500 ms throttle. Cheap -- two raw reflected
        // reads and a mutex'd copy -- and MakePayload returns false until the cycle is streamed in,
        // so we never publish garbage.
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
        // Restore: 1.0 is the game's own TimeScale restore value (daynightCycle ubergraph @10703).
        // Single-player day-rolling resumes from the last synced clock, and the dailyDelivery latch
        // self-heals at the next local midnight.
        g_suppressedClient = false;
        DNC::WriteTimeScale(1.0f);
        UE_LOGI("time_sync: restore -- client TimeScale back to 1.0");
    }
}

}  // namespace coop::time_sync
