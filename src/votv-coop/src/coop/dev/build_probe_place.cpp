// coop/dev/build_probe_place.cpp -- the building probe's wall placement: a wall spawned, put on the ground under a point
// and read back, so a wall that did not land where asked is said and never hit or thrown at; see coop/dev/build_probe.h.

#include "build_probe_detail.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>

namespace coop::dev::build_probe::detail {
namespace {

namespace E = ue_wrap::engine;

constexpr float kTraceAboveCm = 300.f;   // the ground line starts this far above the asked point
constexpr float kTraceBelowCm = 1000.f;  // and ends this far below it
constexpr float kReadBackCm = 50.f;      // the read-back this near the ground point counts as placed

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void* PlaceOnGround(const wchar_t* classPath, const ue_wrap::FVector& ask, const char* what, ue_wrap::FVector& placed) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(classPath);
    void* wall = cls ? E::SpawnActor(cls, ask) : nullptr;
    if (!wall) {
        Invalid("%s did not spawn", what);
        return nullptr;
    }
    const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic
    void* const ignore[1] = {wall};
    ue_wrap::trace::Hit hit;
    const ue_wrap::FVector from{ask.X, ask.Y, ask.Z + kTraceAboveCm}, to{ask.X, ask.Y, ask.Z - kTraceBelowCm};
    if (!ue_wrap::trace::LineHitForObjects(wall, from, to, types, 2, ignore, 1, &hit) || !hit.blocked) {
        E::DestroyActor(wall);
        Invalid("%s: no ground under (%.0f,%.0f,%.0f) within %.0f cm above and %.0f cm below", what,
                static_cast<double>(ask.X), static_cast<double>(ask.Y), static_cast<double>(ask.Z),
                static_cast<double>(kTraceAboveCm), static_cast<double>(kTraceBelowCm));
        return nullptr;
    }
    ue_wrap::FVector back{};
    const bool moved = E::SetActorLocation(wall, hit.point);
    const bool read = E::TryGetActorLocation(wall, back);
    const float off = read ? Dist(back, hit.point) : -1.f;
    Info("place: %s asked (%.1f,%.1f,%.1f), ground hit (%.1f,%.1f,%.1f) on %s, read back (%.1f,%.1f,%.1f), %.1f cm off", what,
         static_cast<double>(ask.X), static_cast<double>(ask.Y), static_cast<double>(ask.Z), static_cast<double>(hit.point.X),
         static_cast<double>(hit.point.Y), static_cast<double>(hit.point.Z), ClassOrNull(hit.actor).c_str(),
         static_cast<double>(back.X), static_cast<double>(back.Y), static_cast<double>(back.Z), static_cast<double>(off));
    if (!moved || !read || off > kReadBackCm) {
        E::DestroyActor(wall);
        Invalid("%s is not where asked: moved=%d read=%d, %.1f cm from the ground point (limit %.0f cm)", what, moved ? 1 : 0,
                read ? 1 : 0, static_cast<double>(off), static_cast<double>(kReadBackCm));
        return nullptr;
    }
    placed = back;
    return wall;
}

}  // namespace coop::dev::build_probe::detail
