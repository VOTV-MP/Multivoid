// coop/time_sync.cpp -- see coop/time_sync.h. Host-authoritative world-clock sync.

#include "coop/time_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/daynightcycle.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>

namespace coop::time_sync {
namespace {

namespace DNC = ue_wrap::daynightcycle;

std::atomic<coop::net::Session*> g_session{nullptr};
std::chrono::steady_clock::time_point g_lastBroadcast{};

// The clock is continuous + the client free-runs its own ReceiveTick at the synced TimeScale,
// so the push is only an anti-drift correction -- a slow rate is plenty and keeps the wire quiet.
constexpr auto kPushInterval = std::chrono::milliseconds(2000);

bool MakePayload(coop::net::TimeSyncPayload& out) {
    float t = 0, d = 0, s = 0;
    if (!DNC::ReadClock(t, d, s)) return false;  // cycle not streamed in yet
    out.totalTime = t; out.day = d; out.timeScale = s;
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
    DNC::ApplyClock(payload.totalTime, payload.day, payload.timeScale);
    static int s_n = 0;
    if ((s_n++ % 5) == 0)  // ~every 10s -- enough to confirm convergence, not spam
        UE_LOGI("time_sync: applied host clock totalTime=%.1f day=%.1f scale=%.3f",
                payload.totalTime, payload.day, payload.timeScale);
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
}

}  // namespace coop::time_sync
