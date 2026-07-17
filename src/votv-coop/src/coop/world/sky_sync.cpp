// coop/sky_sync.cpp -- see coop/sky_sync.h. Host-authoritative night-sky sync (star-dome
// world rotation + moon phase). A near-verbatim clone of coop/time_sync.cpp.

#include "coop/world/sky_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/core/types.h"  // FRotator

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::sky_sync {
namespace {

namespace SKY = ue_wrap::skysphere;

std::atomic<coop::net::Session*> g_session{nullptr};
std::chrono::steady_clock::time_point g_lastBroadcast{};

// The dome spins very slowly (dt/32 deg/frame ~ 0.03 deg/s) + moonPhase is near-static, so a
// ~1 Hz push keeps both visually locked while staying quiet on the wire (a bit faster than the
// 2 s clock push since orientation is more salient than a small time drift).
constexpr auto kPushInterval = std::chrono::milliseconds(1000);

bool Finite(const coop::net::SkyStatePayload& p) {
    return std::isfinite(p.skyPitch) && std::isfinite(p.skyYaw) &&
           std::isfinite(p.skyRoll) && std::isfinite(p.moonPhase);
}

bool MakePayload(coop::net::SkyStatePayload& out) {
    ue_wrap::FRotator rot{};
    float moon = 0.f;
    if (!SKY::ReadSky(rot, moon)) return false;  // sky not streamed in yet
    out.skyPitch = rot.Pitch;
    out.skyYaw   = rot.Yaw;
    out.skyRoll  = rot.Roll;
    out.moonPhase = moon;
    return Finite(out);  // never push NaN/inf onto the wire
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    SKY::EnsureResolved();  // retried via Tick until newsky_C loads
}

void OnReliable(const coop::net::SkyStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // host is authoritative -- never apply a received sky
    if (!Finite(payload)) { UE_LOGW("sky_sync: dropping non-finite payload"); return; }
    SKY::ApplySky(ue_wrap::FRotator{payload.skyPitch, payload.skyYaw, payload.skyRoll}, payload.moonPhase);
    static int s_n = 0;
    if ((s_n++ % 10) == 0)  // ~every 10s -- confirm convergence, not spam
        UE_LOGI("sky_sync: applied host sky yaw=%.1f moonPhase=%.3f", payload.skyYaw, payload.moonPhase);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    coop::net::SkyStatePayload p{};
    if (!MakePayload(p)) return;
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SkyState, &p, sizeof(p));
    UE_LOGI("sky_sync: connect-snapshot -- sent sky (yaw=%.1f moonPhase=%.3f) to slot %d",
            p.skyYaw, p.moonPhase, peerSlot);
}

void Tick() {
    if (!SKY::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;  // host broadcasts; client applies on receipt
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastBroadcast < kPushInterval) return;
    coop::net::SkyStatePayload p{};
    if (!MakePayload(p)) return;
    g_lastBroadcast = now;
    if (s->SendReliable(coop::net::ReliableKind::SkyState, &p, sizeof(p))) {
        static int s_n = 0;
        if ((s_n++ % 10) == 0)  // ~every 10s
            UE_LOGI("sky_sync: host sky yaw=%.1f moonPhase=%.3f", p.skyYaw, p.moonPhase);
    } else {
        UE_LOGW("sky_sync: SendReliable(SkyState) failed");
    }
}

void OnDisconnect() {
    g_lastBroadcast = {};
}

}  // namespace coop::sky_sync
