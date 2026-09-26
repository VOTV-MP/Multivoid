// coop/world/sky_sync.cpp -- see coop/world/sky_sync.h. Host-authoritative night-sky sync
// (star-dome world rotation, moon phase, the sky eye), built on the same shape as coop/world/time_sync.cpp.

#include "coop/world/sky_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/session/net_pump.h"          // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/core/types.h"  // FRotator

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::sky_sync {
namespace {

namespace SKY = ue_wrap::skysphere;
namespace sg  = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};
std::chrono::steady_clock::time_point g_lastBroadcast{};

// The dome spins very slowly (dt/32 deg/frame ~ 0.03 deg/s) and moonPhase is near-static, so a
// ~1 Hz push keeps both visually locked while staying quiet on the wire.
constexpr auto kPushInterval = std::chrono::milliseconds(1000);

// The eye: the day's noon roll runs on every peer and calls the sky's setEye, so a client refuses its own
// and shows the host's, which the lane carries. Game thread.
constexpr int kSetEyeTag = 0x534b4559;  // 'SKEY'
bool     g_gateAsked = false;            // the refusal gate was registered (or refused, said)
bool     g_applyingEye = false;          // the lane's own apply of the host's eye runs
uint64_t g_eyeRefused = 0;               // a client's own setEye calls refused this session
bool     g_saidEyeRefused = false;
uint64_t g_eyeApplyFailed = 0;           // the host's eye samples whose setEye did not run this session

// The lane's own apply, for the length of one setEye call: the gate lets exactly this call run.
struct ApplyingEye {
    ApplyingEye() { g_applyingEye = true; }
    ~ApplyingEye() { g_applyingEye = false; }
    ApplyingEye(const ApplyingEye&) = delete;
    ApplyingEye& operator=(const ApplyingEye&) = delete;
};

// CLIENT, before newsky's setEye: the host's sky owns the eye. The lane's apply runs, and so does a call
// in a world this client has not announced ready, its own load of the host's save.
sg::Verdict OnSetEyePre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    if (g_applyingEye || !call.object) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    ++g_eyeRefused;
    if (!g_saidEyeRefused) {
        g_saidEyeRefused = true;
        UE_LOGI("sky_sync: a client refused its own setEye -- the host's sky owns the eye (first refusal; the "
                "rest are counted)");
    }
    return sg::Verdict::Cancel;
}

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
    bool eye = false;
    SKY::ReadEye(eye);  // unread, the sky shows no eye
    out.eye = eye ? 1 : 0;
    return Finite(out);  // never push NaN/inf onto the wire
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    SKY::EnsureResolved();  // retried via Tick until newsky_C loads
    if (!g_gateAsked) {
        g_gateAsked = true;
        if (!sg::WatchClassName(L"newsky_C", L"setEye", kSetEyeTag, &OnSetEyePre, nullptr))
            UE_LOGE("sky_sync: the script gate refused the watch on newsky_C.setEye -- a client's own noon roll "
                    "can still set its eye");
    }
}

void OnReliable(const coop::net::SkyStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // host is authoritative -- never apply a received sky
    if (!Finite(payload)) { UE_LOGW("sky_sync: dropping non-finite payload"); return; }
    SKY::ApplySky(ue_wrap::FRotator{payload.skyPitch, payload.skyYaw, payload.skyRoll}, payload.moonPhase);
    // The eye through the sky's own setEye, which swaps the moon's texture; the gate lets this call run.
    // A call that did not run is said once and counted; each next sample tries again.
    const bool want = payload.eye != 0;
    bool cur = false;
    if (SKY::ReadEye(cur) && cur != want) {
        bool ok = false;
        {
            ApplyingEye scope;
            ok = SKY::CallSetEye(want);
        }
        if (ok)
            UE_LOGI("sky_sync: applied the host's eye %d", want ? 1 : 0);
        else if (g_eyeApplyFailed++ == 0)
            UE_LOGW("sky_sync: the host's eye %d did not apply -- the sky's setEye did not run (first failure; the "
                    "rest are counted, and each next sample tries again)", want ? 1 : 0);
    }
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
    UE_LOGI("sky_sync: connect-snapshot -- sent sky (yaw=%.1f moonPhase=%.3f eye=%d) to slot %d",
            p.skyYaw, p.moonPhase, p.eye, peerSlot);
}

void Tick() {
    sg::ResolvePendingNames();  // the eye gate's name, on the game thread
    if (!SKY::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;  // host broadcasts; client applies on receipt
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastBroadcast < kPushInterval) return;
    // A joiner is connected for tens of seconds before it has a world, and SkyState is not on the
    // pre-world allowlist, so the fan-out drops it per slot and reports no delivery. That is a
    // vacuous success for this lane, not a failure: QueueConnectBroadcastForSlot seeds the sky the
    // moment the slot goes world-ready. Asking with nobody eligible is what turned a real warning
    // into 24 false alarms per join. Gated after the interval so it costs nothing per frame.
    if (!s->AnyWorldReadyPeer()) return;
    coop::net::SkyStatePayload p{};
    if (!MakePayload(p)) return;
    g_lastBroadcast = now;
    if (s->SendReliable(coop::net::ReliableKind::SkyState, &p, sizeof(p))) {
        static int s_n = 0;
        if ((s_n++ % 10) == 0)  // ~every 10s
            UE_LOGI("sky_sync: host sky yaw=%.1f moonPhase=%.3f eye=%d", p.skyYaw, p.moonPhase, p.eye);
    } else {
        UE_LOGW("sky_sync: SendReliable(SkyState) failed");
    }
}

void OnDisconnect() {
    g_lastBroadcast = {};
    if (g_eyeRefused)
        UE_LOGI("sky_sync: session end -- a client refused %llu setEye call(s) of its own",
                static_cast<unsigned long long>(g_eyeRefused));
    if (g_eyeApplyFailed)
        UE_LOGW("sky_sync: session end -- %llu sample(s) of the host's eye did not apply",
                static_cast<unsigned long long>(g_eyeApplyFailed));
    g_eyeRefused = 0;
    g_saidEyeRefused = false;
    g_eyeApplyFailed = 0;
}

}  // namespace coop::sky_sync
