// coop/drone_sync.cpp -- see coop/drone_sync.h. Delivery drone (Adrone_C) Phase 1 body pose sync.
// Host-authoritative singleton transform stream: the host reads the live drone transform +
// throttled-streams it while Active; the client suppresses the drone's own ReceiveTick (so it is
// purely a mirror) and drives the streamed transform kinematically with a LerpWindow interp.
//
// Singleton (no key) -> the sky_sync host-auth push shape (host streams; client applies; host
// early-returns) + the atv_sync LerpWindow interp. No index (one drone).

#include "coop/interactables/drone_sync.h"

#include "coop/interactables/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/drone.h"
#include "ue_wrap/log.h"
#include "ue_wrap/types.h"  // FVector, FRotator, NormalizeAxis

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::drone_sync {
namespace {

namespace D = ue_wrap::drone;
using ue_wrap::FVector;
using ue_wrap::FRotator;

constexpr uint64_t kSendIntervalMs = 50;   // ~20 Hz host transform stream while the drone is Active
constexpr int      kInterpWindowMs = 75;   // matches the NPC/ATV pose interp window

std::atomic<coop::net::Session*> g_session{nullptr};

// The single drone mirror's receiver-side interp state (game thread only).
struct DroneMirror {
    coop::LerpWindow window;
    FVector curPos{}, tgtPos{}, errPos{};
    float   curPitch = 0.f, tgtPitch = 0.f, errPitch = 0.f;
    float   curYaw   = 0.f, tgtYaw   = 0.f, errYaw   = 0.f;
    float   curRoll  = 0.f, tgtRoll  = 0.f, errRoll  = 0.f;
    bool    hasPose    = false;
    bool    dirty      = false;
    bool    suppressed = false;   // we disabled the client drone's flight tick
    uint8_t lastStateBits = 0;    // last applied FX bits (dust / canTakeOff) -- edge detection
    bool    haveStateBits = false;
};
DroneMirror g_m;

// Host sender edge state (game thread only).
uint64_t g_lastSentMs     = 0;
bool     g_lastSentActive = false;
bool     g_installLogged  = false;  // latch the install log (Install is the per-tick ensure path)

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool FillPayload(void* drone, bool active, bool adopt, coop::net::DroneStatePayload& p) {
    FVector loc; FRotator rot;
    if (!D::GetTransform(drone, loc, rot)) return false;
    std::memset(&p, 0, sizeof(p));
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.active = active ? 1 : 0;
    p.stateBits = D::ReadFxBits(drone);  // bit0=rotor dust active, bit1=canTakeOff (arrived) -- FX mirror
    p.adopt = adopt ? 1 : 0;
    // v69: the dust anchor -- the BP pins the bAbsoluteLocation eff_droneDust
    // to its ground-trace hit per tick; the mirror replays from this. A dust
    // bit without a readable anchor is unusable on the receiver -> clear it.
    if (p.stateBits & D::kFxDust) {
        FVector a;
        if (D::ReadDustAnchor(drone, a)) { p.dustX = a.X; p.dustY = a.Y; p.dustZ = a.Z; }
        else                             { p.stateBits &= ~D::kFxDust; }
    }
    return true;
}

void AdvanceInterp(DroneMirror& e) {
    bool arrived = false;
    const float dA = e.window.Advance(NowMs(), &arrived);
    if (dA > 0.f) {
        e.curPos.X += e.errPos.X * dA;
        e.curPos.Y += e.errPos.Y * dA;
        e.curPos.Z += e.errPos.Z * dA;
        e.curPitch += e.errPitch * dA;
        e.curYaw   += e.errYaw   * dA;
        e.curRoll  += e.errRoll  * dA;
        e.dirty = true;
    }
    if (arrived) {
        e.curPos = e.tgtPos;
        e.curPitch = e.tgtPitch; e.curYaw = e.tgtYaw; e.curRoll = e.tgtRoll;
        e.dirty = true;
    }
}

void SetTarget(DroneMirror& e, const coop::net::DroneStatePayload& p, bool snap) {
    if (!e.hasPose || snap) {
        e.curPos = { p.x, p.y, p.z }; e.tgtPos = e.curPos; e.errPos = {};
        e.curPitch = e.tgtPitch = p.pitch; e.errPitch = 0.f;
        e.curYaw   = e.tgtYaw   = p.yaw;   e.errYaw   = 0.f;
        e.curRoll  = e.tgtRoll  = p.roll;  e.errRoll  = 0.f;
        e.window.Close();
        e.hasPose = true; e.dirty = true;
        return;
    }
    AdvanceInterp(e);  // advance-before-rebase (interp-starvation fix)
    e.tgtPos = { p.x, p.y, p.z };
    e.errPos = { e.tgtPos.X - e.curPos.X, e.tgtPos.Y - e.curPos.Y, e.tgtPos.Z - e.curPos.Z };
    e.tgtPitch = p.pitch; e.errPitch = ue_wrap::NormalizeAxis(p.pitch - e.curPitch);
    e.tgtYaw   = p.yaw;   e.errYaw   = ue_wrap::NormalizeAxis(p.yaw   - e.curYaw);
    e.tgtRoll  = p.roll;  e.errRoll  = ue_wrap::NormalizeAxis(p.roll  - e.curRoll);
    e.window.Open(NowMs(), kInterpWindowMs);
    e.dirty = true;
}

void ApplyMirror(void* drone, DroneMirror& e) {
    if (!e.dirty) return;
    D::DriveMirror(drone, e.curPos, FRotator{ e.curPitch, e.curYaw, e.curRoll });
    e.dirty = false;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Install is the per-tick idempotent ensure path -- latch the log so it fires once, not 60x/s.
    if (!g_installLogged && D::EnsureResolved()) {
        UE_LOGI("drone: drone_sync installed (drone present=%d)", D::Find() != nullptr ? 1 : 0);
        g_installLogged = true;
    }
}

void OnReliable(const coop::net::DroneStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // host is the authority -- never applies
    if (!D::EnsureResolved()) return;
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll) ||
        !std::isfinite(payload.dustX) || !std::isfinite(payload.dustY) || !std::isfinite(payload.dustZ)) {
        UE_LOGW("drone: OnReliable non-finite pose -- dropping");
        return;
    }
    void* drone = D::Find();
    if (!drone) return;  // not streamed in yet -- the next packet applies once it resolves
    if (!g_m.suppressed) { D::SuppressTick(drone); g_m.suppressed = true; }
    SetTarget(g_m, payload, /*snap*/ payload.adopt != 0);
    // FX mirror: the suppressed tick kills the rotor-dust particle + the delivery alarm cue, so
    // replay them off the synced state (host packs it in FillPayload): the dust as a PER-PACKET
    // replay of the BP's own tick update (v69 -- see ApplyDustMirror + drone_dust_notes.md), the
    // "items ready" cue once on the canTakeOff rising edge. All self-contained component calls on
    // the mirror (ue_wrap::drone). Pose RE: votv-delivery-drone-RE-...-2026-06-03.md.
    const uint8_t nb = payload.stateBits;
    const uint8_t ob = g_m.haveStateBits ? g_m.lastStateBits : 0;
    const bool canTakeOff = (nb & D::kFxArrived) != 0;
    const bool hasSack    = (nb & D::kFxHasSack) != 0;

    // Interaction gate: write canTakeOff (THE gate) + hasSack (option prerequisite) onto the mirror so
    // a parked drone is interactable instead of "in motion" (the suppressed tick never sets them).
    D::WriteGateFields(drone, canTakeOff, hasSack);
    if (hasSack)
        D::RepointContainer(drone);  // cargo aboard -> point the mirror at the prop-mirrored container.
                                     // EVERY hasSack packet (not just the rising edge): the container
                                     // may stream in AFTER the edge, or be destroyed+respawned (new
                                     // address) -- RepointContainer short-circuits when the field is
                                     // already a live container, so steady-state cost is ~one deref.

    // Dust (v69): replay the BP's per-tick dust update EVERY packet, not on bit
    // edges -- the anchor moves with the drone, the intensity tracks ground
    // distance, and the EmitterLoops=1/20s system self-completes and needs the
    // IsActive!=want re-arm (all inside ApplyDustMirror; ~3 component calls per
    // 50 ms packet while flying, nothing when the bit is off and already off).
    const bool dustOn = (nb & D::kFxDust) != 0;
    D::ApplyDustMirror(drone, dustOn,
                       FVector{ payload.dustX, payload.dustY, payload.dustZ });
    if (dustOn) {
        static bool s_dustOnLogged = false;
        if (!s_dustOnLogged) {
            s_dustOnLogged = true;
            UE_LOGI("drone: FX mirror -- dust ON (anchor %.0f,%.0f,%.0f)",
                    payload.dustX, payload.dustY, payload.dustZ);
        }
    }
    // Arrival cue + signal light on the canTakeOff edges (the "items ready" sound + the visible light).
    if (canTakeOff && !(ob & D::kFxArrived)) {
        D::PlayArrivalCue(drone);
        D::SetSignalLight(drone, true);
        UE_LOGI("drone: FX mirror -- arrival cue + signal light ON (canTakeOff rising edge)");
    } else if (!canTakeOff && (ob & D::kFxArrived)) {
        D::SetSignalLight(drone, false);
    }
    g_m.lastStateBits = nb;
    g_m.haveStateBits = true;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    void* drone = D::Find();
    if (!drone) { UE_LOGI("drone: connect-snapshot -- no drone present (skip) slot %d", peerSlot); return; }
    coop::net::DroneStatePayload p{};
    if (!FillPayload(drone, D::IsActive(drone), /*adopt*/true, p)) return;
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DroneState, &p, sizeof(p));
    UE_LOGI("drone: connect-snapshot -- sent pose to slot %d (active=%d)", peerSlot, p.active);
}

void Tick() {
    if (!D::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    void* drone = D::Find();
    if (!drone) return;

    if (s->role() == coop::net::Role::Host) {
        // HOST authority: stream the transform while the drone is Active; emit one falling-edge
        // inactive so the client's active flag is accurate (it already holds the last pose).
        const bool active = D::IsActive(drone);
        const uint64_t nowMs = NowMs();
        if (active) {
            if (nowMs - g_lastSentMs >= kSendIntervalMs) {
                g_lastSentMs = nowMs;
                coop::net::DroneStatePayload p{};
                if (FillPayload(drone, true, /*adopt*/false, p))
                    s->SendReliable(coop::net::ReliableKind::DroneState, &p, sizeof(p));
                g_lastSentActive = true;
            }
        } else if (g_lastSentActive) {
            coop::net::DroneStatePayload p{};
            if (FillPayload(drone, false, /*adopt*/false, p))
                s->SendReliable(coop::net::ReliableKind::DroneState, &p, sizeof(p));
            g_lastSentActive = false;
        }
    } else {
        // CLIENT: the drone is ALWAYS a mirror -- suppress its own flight tick once, then drive the
        // interp toward the last streamed pose (no-op when frozen at target between packets).
        if (!g_m.suppressed) { D::SuppressTick(drone); g_m.suppressed = true; }
        if (g_m.hasPose) { AdvanceInterp(g_m); ApplyMirror(drone, g_m); }
    }
}

void OnDisconnect() {
    if (g_m.suppressed) {
        if (void* drone = D::Find()) D::RestoreTick(drone);  // restore single-player flight
    }
    g_m = DroneMirror{};
    g_lastSentMs = 0;
    g_lastSentActive = false;
    g_installLogged = false;
}

}  // namespace coop::drone_sync
