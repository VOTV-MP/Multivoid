// coop/dev/build_probe_place.cpp -- the building probe's wall placement: a ground line trace under a point, the wall
// spawned at its hit and read back, so a wall that did not land where asked is said and never hit or thrown at; and
// the level's own wall, found by class and level; see coop/dev/build_probe.h.

#include "build_probe_detail.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>

namespace coop::dev::build_probe::detail {
namespace {

namespace E = ue_wrap::engine;

constexpr float kTraceAboveCm = 300.f;   // the ground line starts this far above the asked point
constexpr float kTraceBelowCm = 1000.f;  // and ends this far below it
constexpr float kReadBackCm = 50.f;      // the read-back this near the ground hit counts as placed

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void* PlaceOnGround(void* ctx, const wchar_t* classPath, const ue_wrap::FVector& ask, const char* what,
                    ue_wrap::FVector& placed) {
    const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic
    void* const ignore[1] = {ctx};
    ue_wrap::trace::Hit hit;
    const ue_wrap::FVector from{ask.X, ask.Y, ask.Z + kTraceAboveCm}, to{ask.X, ask.Y, ask.Z - kTraceBelowCm};
    if (!ctx || !ue_wrap::trace::LineHitForObjects(ctx, from, to, types, 2, ignore, 1, &hit) || !hit.blocked) {
        Invalid("%s: no ground under (%.0f,%.0f,%.0f) within %.0f cm above and %.0f cm below", what,
                static_cast<double>(ask.X), static_cast<double>(ask.Y), static_cast<double>(ask.Z),
                static_cast<double>(kTraceAboveCm), static_cast<double>(kTraceBelowCm));
        return nullptr;
    }
    void* cls = ue_wrap::asset_load::LoadObjectByPath(classPath);
    void* wall = cls ? E::SpawnActor(cls, hit.point) : nullptr;
    if (!wall) {
        Invalid("%s did not spawn at the ground hit (%.1f,%.1f,%.1f)", what, static_cast<double>(hit.point.X),
                static_cast<double>(hit.point.Y), static_cast<double>(hit.point.Z));
        return nullptr;
    }
    ue_wrap::FVector back{};
    const bool read = E::TryGetActorLocation(wall, back);
    const float off = read ? Dist(back, hit.point) : -1.f;
    Info("place: %s asked (%.1f,%.1f,%.1f), ground hit (%.1f,%.1f,%.1f) on %s, spawned there, read back (%.1f,%.1f,%.1f), %.1f cm off",
         what, static_cast<double>(ask.X), static_cast<double>(ask.Y), static_cast<double>(ask.Z),
         static_cast<double>(hit.point.X), static_cast<double>(hit.point.Y), static_cast<double>(hit.point.Z),
         ClassOrNull(hit.actor).c_str(), static_cast<double>(back.X), static_cast<double>(back.Y),
         static_cast<double>(back.Z), static_cast<double>(off));
    if (!read || off > kReadBackCm) {
        E::DestroyActor(wall);
        Invalid("%s is not where it was spawned: read=%d, %.1f cm from the ground hit (limit %.0f cm)", what, read ? 1 : 0,
                static_cast<double>(off), static_cast<double>(kReadBackCm));
        return nullptr;
    }
    placed = back;
    return wall;
}

namespace {
struct WallFind {
    void* world;
    void* wall = nullptr;
};

void OnWall(void* ctx, void* obj, int32_t) {
    auto* f = static_cast<WallFind*>(ctx);
    if (f->wall || ue_wrap::reflection::NameStartsWith(ue_wrap::reflection::NameOf(obj), L"Default__") ||
        !ue_wrap::reflection::IsLive(obj))
        return;
    void* level = ue_wrap::reflection::OuterOf(obj);
    if (level && ue_wrap::reflection::OuterOf(level) == f->world) f->wall = obj;
}
}  // namespace

void* LevelWall() {
    void* cls = ue_wrap::object_index::ClassByName(L"customWall_C");
    if (!cls) return nullptr;
    WallFind f{ue_wrap::world_identity::CurrentWorld()};
    ue_wrap::object_index::ForEachInstance(cls, &OnWall, &f);
    return f.wall;
}

}  // namespace coop::dev::build_probe::detail
