// coop/interactables/drone_sync.cpp -- see coop/interactables/drone_sync.h. Delivery drone
// (Adrone_C) Phase 1 body pose sync. Host-authoritative singleton transform stream: the host reads
// the live drone transform and streams it, throttled, while it moves or its state changes; the client
// suppresses the drone's
// own ReceiveTick (so it is purely a mirror) and drives the streamed transform kinematically with a
// LerpWindow interp.
//
// Singleton (no key) -> the sky_sync host-auth push shape (host streams; client applies; host
// early-returns) + the atv_sync LerpWindow interp. No index (one drone).

#include "coop/interactables/drone_sync.h"

#include "coop/element/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/devices/drone.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"  // FVector, FRotator, NormalizeAxis

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

constexpr uint64_t kSendIntervalMs = 50;   // ~20 Hz at most, while the drone moves or its state changes
// Past its flight and its dust, how far the drone must be from the pose last sent for a new one to
// go: a glide streams until it is this close to where it rests, and a parked drone sends nothing.
constexpr float    kMoveEpsCm  = 1.0f;
constexpr float    kTurnEpsDeg = 0.5f;
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
    int8_t  hostActive = -1;      // the host's last word on Active: 1, 0, or -1 before any
    // The newest word that arrived before this peer's drone resolved, waiting whole -- the pose and the
    // gate fields with it -- for the first tick that finds the drone.
    bool    hasPending = false;
    coop::net::DroneStatePayload pending{};
};
DroneMirror g_m;

// What the host last sent (game thread only): a pose goes when the drone moved away from it, and a
// state when Active or the FX / gate bits differ from it. The check itself runs at the send cadence,
// so a parked drone costs a few memory reads twenty times a second.
uint64_t g_nextCheckMs = 0;
bool     g_haveSent   = false;
FVector  g_sentLoc{};
FRotator g_sentRot{};
bool     g_sentActive = false;
uint8_t  g_sentBits   = 0;
bool     g_installLogged  = false;  // latch the install log (Install is the per-tick ensure path)

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host's drone as one check reads it, from memory.
struct HostRead {
    FVector  loc{};
    FRotator rot{};
    bool     active = false;
    uint8_t  bits = 0;  // bit0 rotor dust, bit1 canTakeOff (arrived), bit2 hasSack
};

bool ReadHost(void* drone, HostRead& r) {
    if (!D::ReadPose(drone, r.loc, r.rot)) return false;
    r.active = D::IsActive(drone);
    r.bits = D::ReadFxBits(drone);
    return true;
}

void FillPayload(void* drone, const HostRead& r, bool adopt, coop::net::DroneStatePayload& p) {
    std::memset(&p, 0, sizeof(p));
    p.x = r.loc.X; p.y = r.loc.Y; p.z = r.loc.Z;
    p.pitch = r.rot.Pitch; p.yaw = r.rot.Yaw; p.roll = r.rot.Roll;
    p.active = r.active ? 1 : 0;
    p.stateBits = r.bits;
    p.adopt = adopt ? 1 : 0;
    // The dust anchor: the blueprint pins the bAbsoluteLocation eff_droneDust to its ground-trace
    // hit every tick, and the mirror replays from this. A dust bit without a readable anchor is
    // unusable on the receiver, so clear it.
    if (p.stateBits & D::kFxDust) {
        FVector a;
        if (D::ReadDustAnchor(drone, a)) { p.dustX = a.X; p.dustY = a.Y; p.dustZ = a.Z; }
        else                             { p.stateBits &= ~D::kFxDust; }
    }
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

namespace {

// One word of the host's onto this peer's drone: the pose into the interpolation, then the FX and gate
// state. Game thread.
void ApplyState(void* drone, const coop::net::DroneStatePayload& payload) {
    if (!g_m.suppressed) { D::SuppressTick(drone); g_m.suppressed = true; }
    SetTarget(g_m, payload, /*snap*/ payload.adopt != 0);
    // FX mirror: the suppressed tick kills the rotor-dust particle and the delivery alarm cue, so
    // replay them off the synced state the host packs in FillPayload -- the dust as a per-packet
    // replay of the blueprint's own tick update (see ApplyDustMirror below), the "items ready"
    // cue once on the canTakeOff rising edge. All of it is self-contained component calls on the
    // mirror, through ue_wrap::drone.
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

    // Dust: replay the blueprint's per-tick dust update on EVERY packet rather than on bit edges.
    // The anchor moves with the drone, the intensity tracks ground distance, and the
    // EmitterLoops=1/20s system self-completes and needs the IsActive != want re-arm -- all of it
    // inside ApplyDustMirror, about three component calls per 50 ms packet while flying, and
    // nothing when the bit is off and already off.
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

}  // namespace

void OnReliable(const coop::net::DroneStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // host is the authority -- never applies
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll) ||
        !std::isfinite(payload.dustX) || !std::isfinite(payload.dustY) || !std::isfinite(payload.dustZ)) {
        UE_LOGW("drone: OnReliable non-finite pose -- dropping");
        return;
    }
    // The host's word is kept whole even before this peer's drone resolves: a parked drone sends
    // nothing after its connect snapshot, so a word dropped here -- its Active, its pose, its gate
    // fields -- would not come again until the next flight.
    g_m.hostActive = payload.active ? 1 : 0;
    void* drone = D::EnsureResolved() ? D::Find() : nullptr;
    if (!drone) {
        g_m.pending = payload;
        g_m.hasPending = true;
        return;
    }
    g_m.hasPending = false;
    ApplyState(drone, payload);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    void* drone = D::Find();
    if (!drone) { UE_LOGI("drone: connect-snapshot -- no drone present (skip) slot %d", peerSlot); return; }
    HostRead r;
    if (!ReadHost(drone, r)) return;
    coop::net::DroneStatePayload p{};
    FillPayload(drone, r, /*adopt*/true, p);
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
        // HOST authority. While the drone is Active or its dust is on, the stream runs as it always did:
        // the client replays the dust once per packet, and the effect ends itself unless re-armed. Past
        // that, the pose goes while the drone MOVES -- it glides on after Active drops, and a client that
        // stopped hearing at that edge kept its mirror metres short of where it came to rest -- and
        // Active and the FX / gate bits the moment they change, moving or not: a sack put on a parked
        // drone is a change a client's gate fields need.
        const uint64_t nowMs = NowMs();
        if (nowMs < g_nextCheckMs) return;
        g_nextCheckMs = nowMs + kSendIntervalMs;
        HostRead r;
        if (!ReadHost(drone, r)) return;
        const float dx = r.loc.X - g_sentLoc.X, dy = r.loc.Y - g_sentLoc.Y, dz = r.loc.Z - g_sentLoc.Z;
        const bool moved =
            !g_haveSent || dx * dx + dy * dy + dz * dz > kMoveEpsCm * kMoveEpsCm ||
            std::fabs(ue_wrap::NormalizeAxis(r.rot.Pitch - g_sentRot.Pitch)) > kTurnEpsDeg ||
            std::fabs(ue_wrap::NormalizeAxis(r.rot.Yaw - g_sentRot.Yaw)) > kTurnEpsDeg ||
            std::fabs(ue_wrap::NormalizeAxis(r.rot.Roll - g_sentRot.Roll)) > kTurnEpsDeg;
        const bool streaming = r.active || (r.bits & D::kFxDust) != 0;
        if (!streaming && !moved && r.active == g_sentActive && r.bits == g_sentBits) return;
        coop::net::DroneStatePayload p{};
        FillPayload(drone, r, /*adopt*/false, p);
        if (!s->SendReliable(coop::net::ReliableKind::DroneState, &p, sizeof(p))) return;
        g_haveSent = true;
        g_sentLoc = r.loc;
        g_sentRot = r.rot;
        g_sentActive = r.active;
        g_sentBits = r.bits;  // the bits as read: FillPayload drops the dust bit when its anchor is unread
    } else {
        // CLIENT: the drone is ALWAYS a mirror -- suppress its own flight tick once, take a word that
        // waited for the drone to resolve, then drive the interp toward the last streamed pose (no-op
        // when frozen at target between packets).
        if (!g_m.suppressed) { D::SuppressTick(drone); g_m.suppressed = true; }
        if (g_m.hasPending) {
            g_m.hasPending = false;
            ApplyState(drone, g_m.pending);
        }
        if (g_m.hasPose) { AdvanceInterp(g_m); ApplyMirror(drone, g_m); }
    }
}

int HostActive() { return g_m.hostActive; }

void OnDisconnect() {
    if (g_m.suppressed) {
        if (void* drone = D::Find()) D::RestoreTick(drone);  // restore single-player flight
    }
    g_m = DroneMirror{};
    g_nextCheckMs = 0;
    g_haveSent = false;
    g_sentLoc = FVector{};
    g_sentRot = FRotator{};
    g_sentActive = false;
    g_sentBits = 0;
    g_installLogged = false;
}

}  // namespace coop::drone_sync
