// coop/atv_sync.cpp -- see coop/atv_sync.h. ATV/quadbike (AATV_C) Phase 1 body pose sync.
// Occupant-authoritative keyed pose stream: the seated driver reads its live ATV root transform
// + throttled-streams it; the host relays a client driver's pose; receivers mirror it
// kinematically (physics off) with a LerpWindow interp, unless they are the occupant.
//
// Keyed by Key@0x0618 (save-placed, cross-peer stable) -- NOT eid/Element (YAGNI for one
// always-present keyed actor; the grime/window dirt sync made the same divergence vs its element
// blueprint). The index/poll/connect-snapshot shape follows the keyed-interactable modules
// (power_sync/keypad_sync); the per-ATV LerpWindow interp follows element::Npc's pose drive.

#include "coop/atv_sync.h"

#include "coop/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/players_registry.h"   // Registry::Local / LocalPeerId / kMaxPeers

#include "ue_wrap/atv.h"
#include "ue_wrap/engine.h"          // ReadMainPlayerGrabState (grabber authority) + Get/SetActorRootPhysicsVelocity (release)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"           // FVector, FRotator, NormalizeAxis

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto     kRebuildThrottle = std::chrono::seconds(2);
constexpr uint64_t kSendIntervalMs  = 50;   // ~20 Hz occupant stream while seated
constexpr int      kInterpWindowMs  = 75;   // matches the NPC pose interp window

// Per-ATV state: the resolved index (actor/idx) + the receiver-side interp (LerpWindow + cur/
// target/error for pos + full rotation) + the sender throttle. All game-thread only.
struct AtvEntry {
    void*   actor = nullptr;
    int32_t idx   = -1;
    coop::LerpWindow window;
    FVector curPos{}, tgtPos{}, errPos{};
    float   curPitch = 0.f, tgtPitch = 0.f, errPitch = 0.f;
    float   curYaw   = 0.f, tgtYaw   = 0.f, errYaw   = 0.f;
    float   curRoll  = 0.f, tgtRoll  = 0.f, errRoll  = 0.f;
    uint64_t lastSentMs      = 0;
    bool     hasPose         = false;
    bool     dirty           = false;
    bool     preparedAsMirror = false;   // we disabled this ATV's physics/tick to mirror it
    bool     wasGrabber      = false;    // we were the grav-hand grabber last tick (release-edge detect)
};

std::atomic<coop::net::Session*> g_session{nullptr};

// g_atvs is GAME-THREAD ONLY: Install / Tick / OnReliable (event_feed drain) /
// QueueConnectBroadcastForSlot / OnDisconnect all run on the game thread, serially within the
// net-pump, so no synchronization is needed (the drain + the sync ticks never overlap). Tick
// mutates only entry FIELDS; structural inserts/erases happen in RebuildIndex (called at the top
// of Tick, before the iteration) and OnReliable (the drain, before the ticks).
std::unordered_map<std::wstring, AtvEntry> g_atvs;
std::chrono::steady_clock::time_point g_lastRebuild{};
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash  = 0;
bool     g_installed    = false;  // latch the one-time index+log (Install is the per-tick ensure path)

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// True iff THIS peer's local player is currently seated in `actor` -- i.e. we are the driver.
bool IsLocalOccupant(void* actor, void* localPlayer) {
    return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;
}

// True iff THIS peer's local player is currently grav-hand GRABBING `actor` (carrying it in the
// air like an object -- NOT seated). The ATV has no grabbed/held flag of its own (isDriven stays
// false, Player stays null during a grab -- those are written only on the seating path), so the
// grabber identity lives entirely on the player side: mainPlayer.grabbing_actor / holding_actor.
// Dual-field test (mirrors local_streams.cpp's held-prop discipline): the light PhysicsHandle grab
// stamps grabbing_actor; we also accept holding_actor so the predicate can't be wrong-footed by
// which field the engine populates for the ATV's simulating root.
bool IsLocalGrabber(void* actor, void* localPlayer) {
    if (!localPlayer || !actor) return false;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) return false;
    return gs.grabbingActor == actor || gs.holdingActor == actor;
}

// THIS peer is the single authority for `actor` -- it must STREAM it, not mirror it -- iff its
// local player is the driver OR the grav-hand grabber. The two are mutually exclusive (you cannot
// be seated and grav-hand-holding the same ATV at once), so there is still exactly one authority.
bool IsLocalAuthority(void* actor, void* localPlayer) {
    return IsLocalOccupant(actor, localPlayer) || IsLocalGrabber(actor, localPlayer);
}

// Fill an AtvStatePayload from a live ATV read. False if the transform read fails. `grabbed` marks
// the authority as the grav-hand grabber (stateBits bit2) rather than a seated driver -- informational
// for receivers (the explicit AtvRelease drives the un-freeze, not this bit).
bool ReadPayload(void* actor, const std::wstring& key, uint8_t occupantSlot, bool adopt,
                 coop::net::AtvStatePayload& p, bool grabbed = false) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.occupantSlot = occupantSlot;
    uint8_t sb = 0;
    if (A::IsDriven(actor)) sb |= 0x1;
    if (A::GetBrake(actor)) sb |= 0x2;
    if (grabbed)            sb |= 0x4;
    p.stateBits = sb;
    p.adopt = adopt ? 1 : 0;
    return true;
}

// Advance the open interp window to now, applying the cached error fractionally (MTA linear
// interp). On arrival snaps cur=target exactly.
void AdvanceInterp(AtvEntry& e) {
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

// Open a fresh interp window toward `p` (advance-before-rebase: apply the still-open window's
// remaining error first, the proven interp-starvation fix). `snap` (first pose / connect-snapshot)
// jumps exactly. Angle errors are shortest-arc (NormalizeAxis).
void SetTarget(AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap) {
    if (!e.hasPose || snap) {
        e.curPos = { p.x, p.y, p.z }; e.tgtPos = e.curPos; e.errPos = {};
        e.curPitch = e.tgtPitch = p.pitch; e.errPitch = 0.f;
        e.curYaw   = e.tgtYaw   = p.yaw;   e.errYaw   = 0.f;
        e.curRoll  = e.tgtRoll  = p.roll;  e.errRoll  = 0.f;
        e.window.Close();
        e.hasPose = true; e.dirty = true;
        return;
    }
    AdvanceInterp(e);
    e.tgtPos = { p.x, p.y, p.z };
    e.errPos = { e.tgtPos.X - e.curPos.X, e.tgtPos.Y - e.curPos.Y, e.tgtPos.Z - e.curPos.Z };
    e.tgtPitch = p.pitch; e.errPitch = ue_wrap::NormalizeAxis(p.pitch - e.curPitch);
    e.tgtYaw   = p.yaw;   e.errYaw   = ue_wrap::NormalizeAxis(p.yaw   - e.curYaw);
    e.tgtRoll  = p.roll;  e.errRoll  = ue_wrap::NormalizeAxis(p.roll  - e.curRoll);
    e.window.Open(NowMs(), kInterpWindowMs);
    e.dirty = true;
}

void ApplyMirror(AtvEntry& e) {
    if (!e.dirty) return;
    A::DriveMirrorTransform(e.actor, e.curPos, FRotator{ e.curPitch, e.curYaw, e.curRoll });
    e.dirty = false;
}

// Full GUObjectArray walk -> refresh the key->actor index, PRESERVING interp/sender state for keys
// that persist (only actor/idx are updated). Game thread. Logs a keys-hash on change.
size_t RebuildIndex() {
    if (!A::EnsureResolved()) return 0;
    std::vector<std::pair<std::wstring, std::pair<void*, int32_t>>> found;
    found.reserve(4);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !A::IsAtv(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        std::wstring key = A::GetKeyString(obj);
        if (key.empty() || key == L"None") continue;
        found.push_back({ std::move(key), { obj, R::InternalIndexOf(obj) } });
    }
    // Drop entries whose ATV vanished (preserve the rest).
    for (auto it = g_atvs.begin(); it != g_atvs.end();) {
        bool present = false;
        for (auto& f : found) if (f.first == it->first) { present = true; break; }
        it = present ? std::next(it) : g_atvs.erase(it);
    }
    // Update/add (preserve interp/sender state for an existing key).
    uint64_t keysHash = 0;
    for (auto& f : found) {
        keysHash ^= FnvKey(f.first);
        AtvEntry& e = g_atvs[f.first];
        e.actor = f.second.first;
        e.idx   = f.second.second;
    }
    if (g_atvs.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = g_atvs.size();
        g_lastLogHash  = keysHash;
        UE_LOGI("atv: index rebuilt -- %zu live ATV(s), keysHash=0x%016llX "
                "(compare host vs client for cross-peer Key stability)",
                g_atvs.size(), static_cast<unsigned long long>(keysHash));
    }
    return g_atvs.size();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Install is the per-tick idempotent "ensure installed" path (net_pump re-calls it every tick
    // until the class loads) -- latch the one-time initial index + log so we don't full-walk the
    // GUObjectArray + spam the log every tick (the Tick's throttled rebuild owns ongoing indexing).
    if (!g_installed && A::EnsureResolved()) {
        UE_LOGI("atv: indexed %zu ATV(s)", RebuildIndex());
        g_installed = true;
    }
}

void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t /*senderPeerSlot*/) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnReliable empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    // NaN/Inf guard before the kinematic engine writes (event_feed also guards; defensive).
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnReliable non-finite pose -- dropping key='%ls'", key.c_str());
        return;
    }
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- the throttled rebuild will pick it up
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;
    // If WE are the AUTHORITY of this ATV (driving OR grav-hand grabbing it), ignore the incoming
    // pose so a relayed/echoed copy can't fight our live driving/carrying.
    if (IsLocalAuthority(e.actor, coop::players::Registry::Get().Local())) return;
    // Mirror: disable the local rig once (so it can't fight the stream), then open the interp.
    if (!e.preparedAsMirror) { A::PrepareMirror(e.actor); e.preparedAsMirror = true; }
    SetTarget(e, payload, /*snap*/ payload.adopt != 0);
}

void OnAtvRelease(const coop::net::AtvReleasePayload& payload, uint8_t /*senderPeerSlot*/) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnAtvRelease empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    // NaN/Inf guard before the kinematic-off + velocity engine writes (event_feed also guards).
    if (!std::isfinite(payload.linVelX) || !std::isfinite(payload.linVelY) || !std::isfinite(payload.linVelZ) ||
        !std::isfinite(payload.angVelX) || !std::isfinite(payload.angVelY) || !std::isfinite(payload.angVelZ)) {
        UE_LOGW("atv: OnAtvRelease non-finite velocity -- dropping key='%ls'", key.c_str());
        return;
    }
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- nothing to un-freeze
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;
    // If WE are the authority (driving OR grabbing this ATV), we own its physics -- ignore a stale
    // or echoed release so it can't perturb our live carry.
    if (IsLocalAuthority(e.actor, coop::players::Registry::Get().Local())) return;
    // Re-enable physics FIRST, THEN write the launch velocity: a kinematic body (mirror) ignores a
    // velocity write, so the simulate-on must precede it (the PropRelease apply order). Stop driving
    // the interp so the ATV's own simulation carries it from here (arc + land).
    if (e.preparedAsMirror) { A::ReleaseMirror(e.actor); e.preparedAsMirror = false; }
    e.hasPose = false;
    e.window.Close();
    e.dirty = false;
    const FVector lin{ payload.linVelX, payload.linVelY, payload.linVelZ };
    const FVector ang{ payload.angVelX, payload.angVelY, payload.angVelZ };
    ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, lin, ang);
    UE_LOGI("atv: OnAtvRelease key='%ls' -- physics re-enabled + launch velocity applied (|lin|=%.0f cm/s)",
            key.c_str(), std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    RebuildIndex();
    int sent = 0;
    for (auto& kv : g_atvs) {
        if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx)) continue;
        coop::net::AtvStatePayload p{};
        // occupantSlot is Phase-1.5 seating -- send 0xFF here; the joiner snaps the BODY pose
        // (adopt=1). If a client is driving at connect time, its live stream takes over at once.
        if (!ReadPayload(kv.second.actor, kv.first, 0xFF, /*adopt*/true, p)) continue;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::AtvState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("atv: connect-snapshot -- sent %d ATV pose(s) to slot %d (of %zu indexed)",
            sent, peerSlot, g_atvs.size());
}

void Tick() {
    if (!A::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);

    const auto nowTp = std::chrono::steady_clock::now();
    if (nowTp - g_lastRebuild >= kRebuildThrottle) {
        g_lastRebuild = nowTp;
        RebuildIndex();
    }

    if (!s || !s->connected()) return;
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = NowMs();

    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        const bool occupant  = IsLocalOccupant(e.actor, localPlayer);
        const bool grabber   = !occupant && IsLocalGrabber(e.actor, localPlayer);  // mutually exclusive
        const bool authority = occupant || grabber;

        // Grab-release / throw edge: we were the grav-hand grabber last tick and are now neither
        // grabber nor occupant. Tell receivers to re-enable the ATV's physics and inherit its launch
        // velocity (the un-freeze) -- without this the ATV would hang at its last streamed pose on
        // every mirror. (An occupant EXIT is intentionally NOT a release: driving behavior is
        // unchanged; only the new grab-carry adds a release. The final pose streamed last tick rides
        // the same Normal lane, so GNS delivers pose-then-release in order -- no settle delay needed.)
        if (e.wasGrabber && !grabber && !occupant) {
            FVector lin{}, ang{};
            ue_wrap::engine::GetActorRootPhysicsVelocity(e.actor, lin, ang);  // best-effort; zero on fail
            coop::net::AtvReleasePayload rp{};
            WireKeyFromString(kv.first, rp.key);
            rp.linVelX = lin.X; rp.linVelY = lin.Y; rp.linVelZ = lin.Z;
            rp.angVelX = ang.X; rp.angVelY = ang.Y; rp.angVelZ = ang.Z;
            s->SendReliable(coop::net::ReliableKind::AtvRelease, &rp, sizeof(rp));
            UE_LOGI("atv: grab released key='%ls' -- AtvRelease |linVel|=%.0f cm/s",
                    kv.first.c_str(),
                    std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z));
        }
        e.wasGrabber = grabber;

        if (authority) {
            // We own this ATV (driving OR grabbing). If it had been a mirror (physics off), restore
            // its rig so our local driving/carrying works again -- and stop applying any stale interp.
            if (e.preparedAsMirror) {
                A::ReleaseMirror(e.actor);
                e.preparedAsMirror = false;
                e.hasPose = false;
                e.window.Close();
            }
            if (nowMs - e.lastSentMs >= kSendIntervalMs) {
                e.lastSentMs = nowMs;
                coop::net::AtvStatePayload p{};
                const uint8_t occSlot = occupant ? localSlot : uint8_t{0xFF};  // grabber: no seated driver
                if (ReadPayload(e.actor, kv.first, occSlot, /*adopt*/false, p, /*grabbed*/grabber))
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
            }
        } else if (e.hasPose) {
            // Mirror: drive the interp toward the last streamed pose (no-op when frozen at target).
            AdvanceInterp(e);
            ApplyMirror(e);
        }
    }
}

void OnDisconnect() {
    for (auto& kv : g_atvs) {
        if (kv.second.preparedAsMirror && R::IsLiveByIndex(kv.second.actor, kv.second.idx))
            A::ReleaseMirror(kv.second.actor);  // don't leave a streamed ATV frozen in single-player
    }
    const size_t n = g_atvs.size();
    g_atvs.clear();
    g_installed = false;  // a new session re-indexes via the next Install (latched again)
    if (n > 0) UE_LOGI("atv: OnDisconnect -- released mirrors + cleared %zu ATV(s)", n);
}

}  // namespace coop::atv_sync
