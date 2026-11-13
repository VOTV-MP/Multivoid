// coop/interactables/drone_sync.cpp -- see coop/interactables/drone_sync.h. Delivery drone
// (Adrone_C) Phase 1 body pose sync. Host-authoritative singleton transform stream: the host reads
// the live drone transform and streams it, throttled, while it moves or its state changes; the client
// suppresses the drone's
// own ReceiveTick (so it is purely a mirror) and drives the streamed transform kinematically, played
// back behind the host's clock, between the two poses that bracket that moment (see AdvanceInterp).
//
// Singleton (no key) -> the sky_sync host-auth push shape (host streams; client applies). No index
// (one drone).

#include "coop/interactables/drone_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"  // FVector, FRotator, NormalizeAxis

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>

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
// The client shows the drone where the host's was a little while ago, between the two poses either side of
// that moment, each placed at the host's own send time, so the copy keeps the host's speed however the poses
// arrive. How long ago follows the link: the lateness that 95 of the last 100 poses kept within, plus a margin,
// between kMinDelayMs and kMaxDelayMs, and it changes by at most a tenth of the time passing, so playback only
// runs a little fast or slow. Past the newest pose the copy goes on along its last motion for up to
// kExtrapMs, then holds.
constexpr double   kMinDelayMs = 80.0;
constexpr double   kMaxDelayMs = 400.0;
constexpr double   kDelayMarginMs = 15.0;
constexpr double   kExtrapMs = 120.0;
constexpr size_t   kLateRing = 100;
// A pose that follows a quiet stretch longer than this (a parked drone sends nothing) starts from the pose
// before it, held until one send period before it: the copy rests where it rested, then moves.
constexpr double   kQuietGapMs = 150.0;
constexpr size_t   kPoseCap = 16;
// Refused freezes in a row (one per tick) before the refusal is said.
constexpr uint32_t kSuppressWarnAfter = 60;

std::atomic<coop::net::Session*> g_session{nullptr};

// The single drone mirror's receiver-side interp state (game thread only).
struct DroneMirror {
    struct Pose {
        double   hostMs;   // unwrapped host send time
        FVector  loc;
        FRotator rot;
    };
    std::deque<Pose> poses;  // oldest first
    double   offsetMs = 0.0;     // local clock minus host clock, as the least-delayed pose shows it
    double   offsetAtMs = 0.0;   // local time the offset was last taken or relaxed
    double   delayMs = kMinDelayMs;  // how far behind the host's clock the copy plays
    double   wantDelayMs = kMinDelayMs;  // what delayMs follows: TargetDelay, taken when the lateness ring changes
    double   late[kLateRing] = {};   // how far behind the newest pose playback would have needed to be
    size_t   lateN = 0;
    double   arrivedAtMs = 0.0;  // local time the newest pose arrived
    uint32_t playTicks = 0;      // frames played while poses were arriving
    uint32_t starvedTicks = 0;   // of them, frames past the newest pose and its extrapolation: the copy held
    uint32_t lastHostMs = 0;     // the newest pose's raw stamp, to unwrap the next
    double   hostBaseMs = 0.0;   // the unwrapped time of lastHostMs
    FVector  curPos{};
    FRotator curRot{};
    bool    hasPose    = false;
    bool    dirty      = false;
    ue_wrap::CachedObjRef suppressed;   // the drone whose flight tick and body we took out; a successor is frozen afresh
    uint32_t suppressRefused = 0;       // freezes the engine refused since the last one that took
    uint8_t lastStateBits = 0;    // last applied FX bits (dust / canTakeOff) -- edge detection
    bool    haveStateBits = false;
    int8_t  hostActive = -1;      // the host's last word on Active: 1, 0, or -1 before any
    bool    leaveWriteWarned = false;  // the leave timer's failed write, said once a session
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

double NowMsF() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The host's drone as one check reads it, from memory.
struct HostRead {
    FVector  loc{};
    FRotator rot{};
    bool     active = false;
    uint8_t  bits = 0;  // bit0 rotor dust, bit1 arrived, bit2 hasSack, bit3 leaveAfter5min
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
    p.hostMs = static_cast<uint32_t>(NowMs());
    // The dust anchor: the blueprint pins the bAbsoluteLocation eff_droneDust to its ground-trace
    // hit every tick, and the mirror replays from this. A dust bit without a readable anchor is
    // unusable on the receiver, so clear it.
    if (p.stateBits & D::kFxDust) {
        FVector a;
        if (D::ReadDustAnchor(drone, a)) { p.dustX = a.X; p.dustY = a.Y; p.dustZ = a.Z; }
        else                             { p.stateBits &= ~D::kFxDust; }
    }
}

float LerpAngle(float a, float b, float t) { return a + ue_wrap::NormalizeAxis(b - a) * t; }

// The delay 95 of the last poses' lateness kept within, plus the margin.
double TargetDelay(const DroneMirror& e) {
    // Drill red (drone_playback_delay_red_ms, off at 0): a delay that ignores the link, so the drill's fake lag and
    // jitter run the playback past the newest pose and its verdict must be FAIL.
    static const long kRedDelayMs = coop::config::ResolveInt(coop::config_registry::rows::drone_playback_delay_red_ms);
    if (kRedDelayMs > 0) return static_cast<double>(kRedDelayMs);
    const size_t n = std::min(e.lateN, kLateRing);
    if (n < 10) return kMinDelayMs;
    double v[kLateRing];
    std::copy(e.late, e.late + n, v);
    std::nth_element(v, v + (n * 95) / 100, v + n);
    return std::clamp(v[(n * 95) / 100] + kDelayMarginMs, kMinDelayMs, kMaxDelayMs);
}

// The pose the copy shows now: delayMs behind the host's clock, between the poses either side.
void AdvanceInterp(DroneMirror& e) {
    if (e.poses.empty()) return;
    const double now = NowMsF();
    const double dt = now - e.offsetAtMs;
    // The offset may only grow slowly (a millisecond a second), so a run of late poses cannot drag playback
    // later at once, while a lasting rise in the delay is followed.
    e.offsetMs += dt * 0.001;
    e.offsetAtMs = now;
    const double want = e.wantDelayMs;   // the ring changes only when a pose arrives: nothing is sorted for a parked drone
    e.delayMs += std::clamp(want - e.delayMs, -0.1 * dt, 0.1 * dt);
    const double at = now - e.offsetMs - e.delayMs;
    while (e.poses.size() > 2 && e.poses[1].hostMs <= at) e.poses.pop_front();
    FVector loc;
    FRotator rot;
    const DroneMirror::Pose& a = e.poses.front();
    if (e.poses.size() == 1 || at <= a.hostMs) {
        loc = a.loc;
        rot = a.rot;  // one pose, or before the oldest: hold it
    } else {
        const DroneMirror::Pose& b = e.poses[1];
        const double span = b.hostMs - a.hostMs;
        // Between the two, or past the newest along their motion for up to kExtrapMs.
        const double tt = span > 0.0 ? std::min((at - a.hostMs) / span, 1.0 + kExtrapMs / span) : 1.0;
        if (now - e.arrivedAtMs < 500.0) {
            ++e.playTicks;
            if (at > b.hostMs + kExtrapMs) ++e.starvedTicks;
        }
        const float t = static_cast<float>(std::max(tt, 0.0));
        loc = {a.loc.X + (b.loc.X - a.loc.X) * t, a.loc.Y + (b.loc.Y - a.loc.Y) * t, a.loc.Z + (b.loc.Z - a.loc.Z) * t};
        rot = {LerpAngle(a.rot.Pitch, b.rot.Pitch, t), LerpAngle(a.rot.Yaw, b.rot.Yaw, t), LerpAngle(a.rot.Roll, b.rot.Roll, t)};
    }
    if (loc.X != e.curPos.X || loc.Y != e.curPos.Y || loc.Z != e.curPos.Z || rot.Pitch != e.curRot.Pitch ||
        rot.Yaw != e.curRot.Yaw || rot.Roll != e.curRot.Roll) {
        e.curPos = loc;
        e.curRot = rot;
        e.dirty = true;
    }
}

void SetTarget(DroneMirror& e, const coop::net::DroneStatePayload& p, bool snap) {
    const double now = NowMsF();
    const FVector loc{p.x, p.y, p.z};
    const FRotator rot{p.pitch, p.yaw, p.roll};
    if (!e.hasPose || snap) {
        e.poses.clear();
        e.hostBaseMs = static_cast<double>(p.hostMs);
        e.lastHostMs = p.hostMs;
        e.offsetMs = now - e.hostBaseMs;
        e.offsetAtMs = now;
        e.poses.push_back({e.hostBaseMs, loc, rot});
        e.curPos = loc;
        e.curRot = rot;
        e.hasPose = true;
        e.dirty = true;
        e.wantDelayMs = TargetDelay(e);
        return;
    }
    // Unwrap the stamp against the newest; one older than the newest (it cannot be, on one reliable lane) is dropped.
    const int32_t step = static_cast<int32_t>(p.hostMs - e.lastHostMs);
    if (step <= 0) return;
    const double hostMs = e.hostBaseMs + step;
    e.lastHostMs = p.hostMs;
    e.hostBaseMs = hostMs;
    const double off = now - hostMs;
    if (off < e.offsetMs) e.offsetMs = off;
    // How late the newest pose before this one was by now, which is how far behind playback had to be not to
    // run past it: a sample of the link's lateness, unless the host was quiet between the two.
    if (!e.poses.empty() && hostMs - e.poses.back().hostMs <= kQuietGapMs)
        e.late[e.lateN++ % kLateRing] = std::max(0.0, now - e.offsetMs - e.poses.back().hostMs);
    e.wantDelayMs = TargetDelay(e);
    if (!e.poses.empty() && hostMs - e.poses.back().hostMs > kQuietGapMs) {
        DroneMirror::Pose rest = e.poses.back();
        rest.hostMs = hostMs - static_cast<double>(kSendIntervalMs);
        e.poses.push_back(rest);
    }
    e.poses.push_back({hostMs, loc, rot});
    e.arrivedAtMs = now;
    while (e.poses.size() > kPoseCap) e.poses.pop_front();
}

// The flight tick and the body of THIS drone are out of the way: asked whenever the drone is not the one already taken
// out, so a drone reborn in the session is frozen too, and again while the engine has refused (said once a streak).
void EnsureSuppressed(void* drone) {
    if (g_m.suppressed.Is(drone)) return;
    if (D::SuppressTick(drone)) {
        g_m.suppressed.Set(drone);
        g_m.suppressRefused = 0;
        return;
    }
    if (++g_m.suppressRefused == kSuppressWarnAfter)
        UE_LOGW("drone: the mirror's freeze was refused %u times running -- asked again each tick", kSuppressWarnAfter);
}

void ApplyMirror(void* drone, DroneMirror& e) {
    if (!e.dirty) return;
    D::DriveMirror(drone, e.curPos, e.curRot);
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
    EnsureSuppressed(drone);
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
    const bool leaveAfter5min = (nb & D::kFxLeaveAfter5min) != 0;

    // Interaction gate: write canTakeOff (THE gate) + hasSack (option prerequisite) onto the mirror so
    // a parked drone is interactable instead of "in motion" (the suppressed tick never sets them).
    D::WriteGateFields(drone, canTakeOff, hasSack);
    if (!D::WriteLeaveAfter5min(drone, leaveAfter5min) && !g_m.leaveWriteWarned) {
        g_m.leaveWriteWarned = true;
        UE_LOGW("drone_sync: the host's leave timer could not be written onto this copy's drone");
    }
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
    if (!s) return;  // from the host alone: its receive edge refuses a client's copy
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
        // CLIENT: the drone is ALWAYS a mirror -- take its flight tick and body out (per drone), take a word that
        // waited for the drone to resolve, then play the streamed poses back (no write while the
        // pose shown does not change).
        EnsureSuppressed(drone);
        if (g_m.hasPending) {
            g_m.hasPending = false;
            ApplyState(drone, g_m.pending);
        }
        if (g_m.hasPose) { AdvanceInterp(g_m); ApplyMirror(drone, g_m); }
    }
}

int HostActive() { return g_m.hostActive; }

MirrorStats Stats() { return {g_m.playTicks, g_m.starvedTicks, g_m.delayMs}; }

void OnDisconnect() {
    if (g_m.playTicks)
        UE_LOGI("drone: session end -- the mirror played %u frames from arriving poses, %u of them past the newest (held); "
                "playback delay %.0f ms", g_m.playTicks, g_m.starvedTicks, g_m.delayMs);
    if (void* drone = g_m.suppressed.Get()) D::RestoreTick(drone);  // restore single-player flight
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
