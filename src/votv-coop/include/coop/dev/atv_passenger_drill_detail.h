// coop/dev/atv_passenger_drill_detail.h -- what the ATV passenger drill's files share: its bounds, the clock and the
// lines the two peers say, the geometry log and the leave check. The host half is atv_passenger_drill_host.cpp, the
// client half and the entry points atv_passenger_drill.cpp.
// Impl-private: nothing outside coop/dev includes it. Game thread.

#pragma once

#include "atv_seat.h"        // TrySit (co-located private header)
#include "atv_spawn_near.h"  // SpawnAtNav, KeyOf (co-located private header)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/puppet_seat.h"
#include "coop/session/teleport_client.h"
#include "coop/dev/director/clear_hand.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/set_clock.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_passenger.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "harness/screenshot.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_passenger.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::atv_passenger_drill::detail {

namespace A  = ue_wrap::atv;
namespace AP = ue_wrap::atv_passenger;
namespace AU = coop::atv_authority;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace SN = coop::dev::atv_spawn_near;
using ue_wrap::FRotator;
using ue_wrap::FVector;
using coop::net::AtvPassengerRefusal;

constexpr const char* kTag = "[ATVPASS-DRILL]";
constexpr uint64_t kBoundMs = 240000;        // a peer waiting on the other's line or the world's edge
constexpr uint64_t kShortMs = 20000;         // an effect that must land on a tick or two
constexpr float kAheadCm = 600.f;            // the ATV is born this far in front of the host's player
constexpr float kWalkReachCm = 330.f;        // the client's walk ends this close to the ATV's root
constexpr int   kWalkDeadlineS = 300;
constexpr float kDriveCm = 800.f;            // the host drives this far
constexpr float kRideTolCm = 60.f;           // a rider's body against its seat on the machine's own copy, at any moment of the ride
constexpr float kFollowFrac = 0.7f;          // the client's body must have moved this share of the drive
constexpr float kParkedCmS = 5.f;
constexpr float kAwayCm = 5000.f;            // the out-of-reach arm moves the host's ATV this far
constexpr uint64_t kBucketGapMs = 1600;      // the host's board bucket refills 1 a second: red requests are spaced past it
constexpr uint8_t kRawSeqBase = 200;         // a raw request's seq: never the client's own pending one

inline bool Enabled() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::atv_passenger_drill) == "on";
    return on;
}

inline uint64_t Now() { return ::GetTickCount64(); }

inline float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline float HorizDist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

inline std::string Num(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.0f", v); return b; }

// ---- the shared clock and lines ---------------------------------------------------------------------------

struct Shared {
    int      step = 0;
    uint64_t stepAt = 0;
    bool     done = false;
    bool     flag = false;      // a one-shot inside a step
    bool     logged = false;    // a line said once while a step waits
    uint64_t flagAt = 0;
};

inline void Next(Shared& g) { ++g.step; g.stepAt = Now(); g.flag = false; g.logged = false; }
inline bool Timed(const Shared& g, uint64_t bound) { return Now() - g.stepAt > bound; }
inline bool Seen(const char* line) { return DC::SeenHas(line); }

inline void Pass(Shared& g, const char* role, const std::string& why) {
    UE_LOGI("%s %s PASS -- %s", kTag, role, why.c_str());
    g.done = true;
}
inline void Fail(Shared& g, const char* role, const std::string& why) {
    UE_LOGE("%s %s FAIL -- %s", kTag, role, why.c_str());
    DC::Say(std::string("atvpass ") + role + " FAIL");
    g.done = true;
}

// The ATV the lane's table holds under `key` on this machine, and its root.
inline bool AtvOf(const std::wstring& key, void** atv, FVector* root = nullptr) {
    coop::atv_sync::TableView t;
    if (!coop::atv_sync::ReadTable(key, t) || !t.actor) return false;
    *atv = t.actor;
    FRotator rot{};
    return !root || A::GetRootTransform(t.actor, *root, rot);
}

inline coop::atv_sync::TableView TableOf(const std::wstring& key) {
    coop::atv_sync::TableView t;
    coop::atv_sync::ReadTable(key, t);
    return t;
}

// The geometry the seat offsets are measured from: the rig's bounds, its seat and cargo components and the player's
// capsule, each in the rig's own axes (X forward, Y right, Z up) from its root.
inline void LogGeometry(void* atv, void* player) {
    FVector root{};
    FRotator rot{};
    if (!A::GetRootTransform(atv, root, rot)) return;
    const FVector f = E::GetActorForwardVector(atv), u = E::GetActorUpVector(atv);
    const FVector r{u.Y * f.Z - u.Z * f.Y, u.Z * f.X - u.X * f.Z, u.X * f.Y - u.Y * f.X};   // up x forward: the right axis
    auto local = [&](const FVector& w) {
        const FVector d{w.X - root.X, w.Y - root.Y, w.Z - root.Z};
        return FVector{d.X * f.X + d.Y * f.Y + d.Z * f.Z, d.X * r.X + d.Y * r.Y + d.Z * r.Z, d.X * u.X + d.Y * u.Y + d.Z * u.Z};
    };
    const FVector sc = E::GetActorScale3D(atv);
    UE_LOGI("%s geom: root (%.0f,%.0f,%.0f) rot (%.1f,%.1f,%.1f) scale (%.2f,%.2f,%.2f)", kTag, root.X, root.Y, root.Z, rot.Pitch,
            rot.Yaw, rot.Roll, sc.X, sc.Y, sc.Z);
    for (const bool colliding : {false, true}) {
        FVector o{}, ext{};
        if (E::GetActorBounds(atv, colliding, o, ext)) {
            const FVector lo = local(o);
            UE_LOGI("%s geom: bounds(%s) origin local (%.0f,%.0f,%.0f) extent world (%.0f,%.0f,%.0f)", kTag,
                    colliding ? "colliding" : "all", lo.X, lo.Y, lo.Z, ext.X, ext.Y, ext.Z);
        }
    }
    void* cls = R::ClassOf(atv);
    for (const wchar_t* name : {L"sitarrow", L"sitkerf", L"playermodel", L"boundingBox", L"hitbox", L"cam", L"cameraRoot",
                                L"overlapBox", L"collision", L"lookatBox", L"Box", L"Box1", L"Scene", L"Arrow", L"Arrow1", L"Arrow2",
                                L"mesh_playerVisible"}) {
        const int32_t off = cls ? R::FindPropertyOffset(cls, name) : -1;
        void* comp = off >= 0 ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atv) + off) : nullptr;
        FVector w{};
        if (!comp || !R::IsLive(comp) || !E::TryGetComponentLocation(comp, w)) {
            UE_LOGI("%s geom: component %ls -- %s", kTag, name, off < 0 ? "no such property" : "not live");
            continue;
        }
        const FVector lw = local(w);
        const FVector rel = E::GetComponentRelativeLocation(comp);
        UE_LOGI("%s geom: component %ls (%ls) local (%.0f,%.0f,%.0f) relative (%.0f,%.0f,%.0f)", kTag, name,
                R::ClassNameOf(comp).c_str(), lw.X, lw.Y, lw.Z, rel.X, rel.Y, rel.Z);
    }
    // Every primitive component the rig owns, with its bounds in the rig's own axes: the body mesh is the rack's measure.
    void* comps[48];
    const int n = E::OwnPrimitiveComponents(atv, comps, 48);
    for (int i = 0; i < n; ++i) {
        void* comp = comps[i];
        const int32_t boff = R::FindPropertyOffset(R::ClassOf(comp), L"Bounds");
        if (boff < 0) continue;
        const float* b = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(comp) + boff);   // FBoxSphereBounds: origin, extent, radius
        const FVector lo = local({b[0], b[1], b[2]});
        UE_LOGI("%s geom: primitive %ls (%ls) bounds origin local (%.0f,%.0f,%.0f) extent (%.0f,%.0f,%.0f)", kTag,
                R::ToString(R::NameOf(comp)).c_str(), R::ClassNameOf(comp).c_str(), lo.X, lo.Y, lo.Z, b[3], b[4], b[5]);
    }
    if (player) {
        FVector at{};
        E::TryGetActorLocation(player, at);
        UE_LOGI("%s geom: the player's capsule half height %.0f radius %.0f, at (%.0f,%.0f,%.0f)", kTag,
                E::GetActorCharacterHalfHeight(player), E::GetActorCharacterRadius(player), at.X, at.Y, at.Z);
    }
    // The rig's own surface under each place a body is put: a trace straight down through the place, from above the rig, that
    // may hit the rig itself (only the player is ignored). A seat's feet rest at its centre minus the capsule's half height.
    if (player) {
        const uint8_t types[4] = {0, 1, 3, 4};   // WorldStatic, WorldDynamic, PhysicsBody, Vehicle
        void* const ignore[1] = {player};
        const FVector places[4] = {{-25.f, 0.f, 0.f}, AP::SeatOffset(0), AP::SeatOffset(1), AP::SeatOffset(2)};
        const char* names[4] = {"the driver's place", "seat 0", "seat 1", "seat 2"};
        for (int i = 0; i < 4; ++i) {
            const FVector top{root.X + f.X * places[i].X + r.X * places[i].Y + u.X * 300.f, root.Y + f.Y * places[i].X + r.Y * places[i].Y + u.Y * 300.f,
                              root.Z + f.Z * places[i].X + r.Z * places[i].Y + u.Z * 300.f};
            const FVector bottom{top.X - u.X * 500.f, top.Y - u.Y * 500.f, top.Z - u.Z * 500.f};
            ue_wrap::trace::Hit hit;
            if (ue_wrap::trace::LineHitForObjects(player, top, bottom, types, 4, ignore, 1, &hit) && hit.blocked) {
                const FVector lh = local(hit.point);
                UE_LOGI("%s geom: %s: the surface under it is at local z %.0f (hit %ls, %s the rig), a rider's centre sits %.0f above it", kTag, names[i], lh.Z,
                        hit.actor ? R::ClassNameOf(hit.actor).c_str() : L"nothing", hit.actor == atv ? "is" : "is not", places[i].Z - lh.Z);
            } else {
                UE_LOGI("%s geom: %s: the trace down found no surface", kTag, names[i]);
            }
        }
    }
    // The rig's surface over a grid behind the driver (local z of the first hit; -- is none), a row per X, the columns Y -60 to 60 by 20.
    if (player) {
        const uint8_t types[4] = {0, 1, 3, 4};
        void* const ignore[1] = {player};
        for (const float gx : {-40.f, -60.f, -80.f, -100.f, -120.f, -140.f}) {
            char row[160] = {};
            size_t used = 0;
            for (float gy = -60.f; gy <= 60.f; gy += 20.f) {
                const FVector top{root.X + f.X * gx + r.X * gy + u.X * 300.f, root.Y + f.Y * gx + r.Y * gy + u.Y * 300.f, root.Z + f.Z * gx + r.Z * gy + u.Z * 300.f};
                const FVector bottom{top.X - u.X * 500.f, top.Y - u.Y * 500.f, top.Z - u.Z * 500.f};
                ue_wrap::trace::Hit hit;
                char cell[16];
                if (ue_wrap::trace::LineHitForObjects(player, top, bottom, types, 4, ignore, 1, &hit) && hit.blocked && hit.actor == atv)
                    std::snprintf(cell, sizeof(cell), "%4.0f", local(hit.point).Z);
                else
                    std::snprintf(cell, sizeof(cell), "  --");
                used += static_cast<size_t>(std::snprintf(row + used, sizeof(row) - used, "%s ", cell));
            }
            UE_LOGI("%s geom: surface grid x=%4.0f, y -60..60: %s", kTag, gx, row);
        }
    }
    for (int i = 0; i < AP::kSeats; ++i) {
        FVector w{};
        if (AP::SeatWorldLocation(atv, i, w)) {
            const FVector lw = local(w);
            UE_LOGI("%s geom: seat %d offset (%.0f,%.0f,%.0f) -> world (%.0f,%.0f,%.0f), local back (%.0f,%.0f,%.0f)", kTag, i,
                    AP::SeatOffset(i).X, AP::SeatOffset(i).Y, AP::SeatOffset(i).Z, w.X, w.Y, w.Z, lw.X, lw.Y, lw.Z);
        }
    }
}

// Where the driver's body is while it sits: the player's actor and its camera, in the rig's own axes from its root.
inline void LogDriverSeat(void* atv, void* player) {
    FVector root{}, at{};
    FRotator rot{};
    if (!A::GetRootTransform(atv, root, rot) || !E::TryGetActorLocation(player, at)) return;
    const FVector f = E::GetActorForwardVector(atv), u = E::GetActorUpVector(atv);
    const FVector r{u.Y * f.Z - u.Z * f.Y, u.Z * f.X - u.X * f.Z, u.X * f.Y - u.Y * f.X};
    auto local = [&](const FVector& w) {
        const FVector d{w.X - root.X, w.Y - root.Y, w.Z - root.Z};
        return FVector{d.X * f.X + d.Y * f.Y + d.Z * f.Z, d.X * r.X + d.Y * r.Y + d.Z * r.Z, d.X * u.X + d.Y * u.Y + d.Z * u.Z};
    };
    const FVector la = local(at), lc = local(E::GetCameraLocation());
    UE_LOGI("%s geom: the seated driver's actor local (%.0f,%.0f,%.0f), its camera local (%.0f,%.0f,%.0f)", kTag, la.X, la.Y, la.Z, lc.X,
            lc.Y, lc.Z);
}

// The use key, at the real recognition path: the aim set at `aimAt` (null: nothing), then InpActEvt_use dispatched on the player.
inline bool PressUse(void* player, void* aimAt) {
    void* cls = R::FindClass(P::name::MainPlayerClass);
    void* fn = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
    if (!fn) return false;
    E::WriteMainPlayerLookAtActor(player, aimAt);
    const int32_t fs = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
    return R::CallFunction(player, fn, frame.empty() ? nullptr : frame.data());
}

// A body that has just left a seat, judged: beside the ATV (the rig may roll on without its driver, so it is measured at the moment of the leave
// and not later) and on the ground; then, a moment on, it must not have fallen or been pushed.
struct LeaveCheck {
    uint64_t atMs = 0;
    FVector  at{};
    float    horiz = 0.f, up = 0.f;
    bool     floorAtLeave = false;
    std::string why;
};

// 0 while it waits, 1 when the body stood, -1 when it did not (`c.why`).
inline int CheckLeft(LeaveCheck& c, void* player, void* atv) {
    FVector root{}, at{};
    FRotator rot{};
    if (!A::GetRootTransform(atv, root, rot) || !E::TryGetActorLocation(player, at)) { c.why = "the ATV or the body does not read after the leave"; return -1; }
    E::FloorRead fr;
    const bool floorNow = E::ReadCurrentFloor(player, &fr) && fr.blocking && fr.walkable;
    if (c.atMs == 0) {
        c.atMs = Now();
        c.at = at;
        c.horiz = HorizDist(at, root);
        c.up = at.Z - root.Z;
        c.floorAtLeave = floorNow;
        return 0;
    }
    if (Now() - c.atMs < 1800) return 0;
    const float dz = at.Z - c.at.Z, slid = HorizDist(at, c.at);
    if (c.horiz >= 100.f && c.horiz <= 450.f && std::fabs(c.up) < 200.f && std::fabs(dz) < 40.f && slid < 60.f && floorNow) return 1;
    c.why = "after the leave the body was " + Num(c.horiz) + " cm from the ATV and " + Num(c.up) + " cm above its root; a moment later it had moved " + Num(dz) +
            " cm in height and " + Num(slid) + " across, floor under it: " + std::to_string(floorNow ? 1 : 0);
    return -1;
}

// The host half's tick, whether it has finished, and its state back to the start (atv_passenger_drill_host.cpp).
void HostTick(coop::net::Session* s);
bool HostDone();
void HostReset();

}  // namespace coop::dev::atv_passenger_drill::detail
