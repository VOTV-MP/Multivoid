// coop/player/pose_bases.cpp -- see coop/player/pose_bases.h.

#include "coop/player/pose_bases.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/interactable_sync.h"

#include "ue_wrap/actors/character_base.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::pose_bases {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace CB = ue_wrap::character_base;

// FNV-1a 32 over the key's UTF-16 code units, low byte first: one derivation on every peer.
uint32_t Fnv32(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (const wchar_t c : s) {
        h = (h ^ static_cast<uint32_t>(c & 0xFF)) * 16777619u;
        h = (h ^ static_cast<uint32_t>((c >> 8) & 0xFF)) * 16777619u;
    }
    return h;
}

// ---- the map hash -> lift ---------------------------------------------------------------------------------------------------
struct Entry {
    uint32_t id = 0;
    ue_wrap::CachedObjRef lift;
    bool duplicate = false;   // two lifts on this hash: unusable
};
std::vector<Entry> g_map;
bool g_built = false;
uint64_t g_passes = 0;
uint32_t g_gen = 0;

// Rebuilt from the lift lane's index when it has completed a pass or the world has changed since (the hub pass rebuilds the
// index from scratch on a full pass, interactable_channel.h HubPassComplete; a stale-world index lists nothing).
void EnsureMap() {
    const uint64_t passes = coop::interactable_sync::CargoLiftIndexPasses();
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (g_built && passes == g_passes && gen == g_gen) return;
    g_built = true;
    g_passes = passes;
    g_gen = gen;
    g_map.clear();
    std::vector<std::pair<std::wstring, void*>> lifts;
    coop::interactable_sync::LiveCargoLifts(lifts);
    for (const auto& kv : lifts) {
        const uint32_t id = Fnv32(kv.first);
        bool dup = false;
        for (Entry& e : g_map)
            if (e.id == id) {
                e.duplicate = dup = true;
                UE_LOGW("pose_bases: two cargo lifts hash to %08X -- that base is sent and resolved as none", id);
            }
        if (dup) continue;
        g_map.emplace_back();
        g_map.back().id = id;
        g_map.back().lift.Set(kv.second);
    }
}

const Entry* Find(uint32_t id) {
    EnsureMap();
    for (const Entry& e : g_map)
        if (e.id == id) return &e;
    return nullptr;
}

// ---- the sender's reading of one movement base ------------------------------------------------------------------------------
// The verdict on the component the local player's engine last named, kept so a player standing on level ground (a base the
// table does not list) costs one pointer compare per pose.
struct SenderCache {
    ue_wrap::CachedObjRef comp;
    bool mover = false;
    uint32_t id = 0;
};
SenderCache g_sender;
std::vector<std::wstring> g_saidClasses;   // the classes said as sent-as-none, once each

void SaidNone(void* owner) {
    const std::wstring cls = owner ? R::ClassNameOf(owner) : std::wstring(L"?");
    for (const std::wstring& c : g_saidClasses)
        if (c == cls) return;
    if (g_saidClasses.size() >= 16) return;   // the list is full: no more classes are said (a log line per base change otherwise)
    g_saidClasses.push_back(cls);
    UE_LOGI("pose_bases: the local player stands on a %ls, which is no registered mover: its pose names no base", cls.c_str());
}

// The table's row 1 for `base`: the cargo lift whose platform the engine reports. 1 a lift, 0 every other base, -1 a lift the
// lane has not indexed yet (not cached: asked again at the next pose).
int LiftRow(const CB::Base& base, uint32_t& id) {
    if (!base.owner || !CL::EnsureResolved()) return 0;
    void* const parent = E::ParentActorOf(base.owner);
    if (!parent || !CL::IsCargoLift(parent) || CL::PlatformActor(parent) != base.owner) return 0;
    const std::wstring key = coop::interactable_sync::CargoLiftKey(parent);
    if (key.empty()) return -1;
    id = Fnv32(key);
    return 1;
}

}  // namespace

bool Resolve(uint32_t baseId, Resolved& out) {
    const Entry* const e = Find(baseId);
    if (!e || e->duplicate) return false;
    void* const lift = e->lift.Get();
    void* const platform = lift ? CL::PlatformActor(lift) : nullptr;
    if (!platform || !E::TryGetActorLocation(platform, out.origin)) return false;
    out.lift = lift;
    out.actor = platform;
    return true;
}

namespace {

constexpr int kReasons = 5;
uint32_t g_rejected[coop::players::kMaxPeers][kReasons] = {};
uint32_t g_accepted[coop::players::kMaxPeers] = {};
Forge g_forge = nullptr;
std::chrono::steady_clock::time_point g_saidAt[coop::players::kMaxPeers] = {};

void SetAside(uint8_t slot, Reject why, const char* what, float a, float b, float c) {
    if (slot >= coop::players::kMaxPeers) return;
    ++g_rejected[slot][static_cast<int>(why)];
    const auto now = std::chrono::steady_clock::now();
    if (g_saidAt[slot].time_since_epoch().count() != 0 && now - g_saidAt[slot] < std::chrono::minutes(1)) return;
    g_saidAt[slot] = now;
    UE_LOGW("pose_bases: slot %u's base claim set aside (%s: %.0f, %.0f, %.0f); it is placed by its absolute pose "
            "(counts xy=%u offsetZ=%u volume=%u zGap=%u bounds=%u)", static_cast<unsigned>(slot), what, a, b, c,
            g_rejected[slot][0], g_rejected[slot][1], g_rejected[slot][2], g_rejected[slot][3], g_rejected[slot][4]);
}

}  // namespace

uint32_t Rejected(uint8_t slot, Reject why) {
    return slot < coop::players::kMaxPeers ? g_rejected[slot][static_cast<int>(why)] : 0u;
}

uint32_t Accepted(uint8_t slot) { return slot < coop::players::kMaxPeers ? g_accepted[slot] : 0u; }

void SetOutboundForge(Forge fn) { g_forge = fn; }

bool Claim(const coop::net::PoseSnapshot& pose, uint8_t slot, Resolved& out) {
    if (pose.baseKind != coop::net::pose_base_kind::kMover || !Resolve(pose.baseId, out)) return false;
    // The XY of (platform now + offset) against the absolute XY: the lift never moves in XY, so link lag cannot widen it.
    const float gx = pose.x - (out.origin.X + pose.bx), gy = pose.y - (out.origin.Y + pose.by);
    const float gap = std::sqrt(gx * gx + gy * gy);
    if (gap > kMaxXyGapCm) {
        SetAside(slot, Reject::XyGap, "xy gap, offset z, abs z", gap, pose.bz, pose.z);
        return false;
    }
    if (pose.bz < 0.f || pose.bz > kMaxOffsetZCm) {
        SetAside(slot, Reject::OffsetZ, "xy gap, offset z, abs z", gap, pose.bz, pose.z);
        return false;
    }
    // The absolute Z against the lift's travel, read on this peer from the lift itself.
    float low = 0.f, high = 0.f;
    if (!CL::ReadStopsZ(out.lift, low, high) || pose.z < low - kBelowStopCm || pose.z > high + kAboveStopCm) {
        SetAside(slot, Reject::Volume, "abs z, low stop, high stop", pose.z, low, high);
        return false;
    }
    // The claimed Z against the absolute Z: the platform moves at the lift's own peak speed at most, so a gap past a second of it is
    // no link lag. A speed that does not read fails closed.
    float peak = 0.f;
    const float zGap = std::fabs(pose.z - (out.origin.Z + pose.bz));
    if (!CL::ReadMaxSpeedCmS(out.lift, peak) || zGap > peak * kMaxZGapSeconds) {
        SetAside(slot, Reject::ZGap, "z gap, peak speed, offset z", zGap, peak, pose.bz);
        return false;
    }
    // The offset lies inside the platform's own bounds (the XY of where the claim puts the player) plus a margin: feet on or over the
    // slab. The bounds are read now, as the platform stands.
    ue_wrap::FVector centre{}, extent{};
    if (!E::GetActorBounds(out.actor, true, centre, extent) ||
        std::fabs(out.origin.X + pose.bx - centre.X) > extent.X + kBoundsMarginCm ||
        std::fabs(out.origin.Y + pose.by - centre.Y) > extent.Y + kBoundsMarginCm) {
        SetAside(slot, Reject::OffsetBounds, "offset x, offset y, half x", pose.bx, pose.by, extent.X);
        return false;
    }
    if (slot < coop::players::kMaxPeers) ++g_accepted[slot];
    return true;
}

void FillBase(void* localPlayer, coop::net::PoseSnapshot& pose) {
    pose.baseId = 0;
    pose.bx = pose.by = pose.bz = 0.f;
    pose.baseKind = coop::net::pose_base_kind::kNone;
    pose._pad[0] = pose._pad[1] = pose._pad[2] = 0;
    // [dev red] basedmove_base_off: the pose names no base, so the receiver places the rider by its absolute pose alone.
    static const bool baseOff = coop::config::ResolveFlag(::coop::config_registry::rows::basedmove_base_off);
    if (baseOff) return;

    // The component alone first: the verdict on it is cached, so a player standing where it stood costs one pointer compare.
    void* const comp = CB::MovementBaseComponent(localPlayer);
    if (!comp) {
        g_sender.comp.Reset();
        return;
    }
    if (g_sender.comp.Get() != comp) {
        CB::Base base;
        if (!CB::ReadMovementBase(localPlayer, base) || base.component != comp) return;
        const int row = LiftRow(base, g_sender.id);
        if (row < 0) return;   // a lift not indexed yet: none for now, asked again at the next pose
        g_sender.comp.Set(base.component);
        g_sender.mover = row == 1;
        if (!g_sender.mover) SaidNone(base.owner);
    }
    if (!g_sender.mover) return;
    // The map must hold the hash and uniquely, or the receiver could not resolve it either.
    Resolved at;
    if (!Resolve(g_sender.id, at)) return;
    pose.baseKind = coop::net::pose_base_kind::kMover;
    pose.baseId = g_sender.id;
    pose.bx = pose.x - at.origin.X;
    pose.by = pose.y - at.origin.Y;
    pose.bz = pose.z - at.origin.Z;
    if (g_forge) g_forge(pose);
}

}  // namespace coop::pose_bases
