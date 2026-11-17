// coop/dev/build_probe_aim.cpp -- the building probe's camera and its bricks: the local player's view turned to a point,
// as the director's drills turn it for a swing or a throw, and the generic props whose row is `brick`; see
// coop/dev/build_probe.h.

#include "build_probe_detail.h"

#include "coop/dev/director/director.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::build_probe::detail {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

void FaceTowards(void* player, const ue_wrap::FVector& at) {
    void* ctrl = player ? E::GetController(player) : nullptr;
    if (ctrl) E::SetControlRotation(ctrl, coop::director::LookAt(E::GetCameraLocation(), at));
}

// The camera's forward lies within five degrees of the direction to `at`: the control rotation FaceTowards wrote has
// reached the view.
bool ViewOn(const ue_wrap::FVector& at) {
    constexpr float kDegToRad = 0.01745329252f;
    constexpr float kToleranceDeg = 5.f;
    const ue_wrap::FVector cam = E::GetCameraLocation();
    const float dx = at.X - cam.X, dy = at.Y - cam.Y, dz = at.Z - cam.Z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(len > 1.f)) return true;
    const ue_wrap::FRotator r = E::GetCameraRotation();
    const float p = r.Pitch * kDegToRad, y = r.Yaw * kDegToRad;
    const float dot = (std::cos(p) * std::cos(y) * dx + std::cos(p) * std::sin(y) * dy + std::sin(p) * dz) / len;
    return dot >= std::cos(kToleranceDeg * kDegToRad);
}

namespace {

void OnProp(void* ctx, void* obj, int32_t) {
    if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    if (ue_wrap::prop::GetPropNameString(obj) != L"brick") return;
    static_cast<std::vector<void*>*>(ctx)->push_back(obj);
}

}  // namespace

void SnapshotBricks(std::vector<void*>& out) {
    out.clear();
    void* cls = ue_wrap::object_index::ClassByName(L"prop_C");
    if (cls) ue_wrap::object_index::ForEachInstance(cls, &OnProp, &out);
}

void* NewBrick(const std::vector<void*>& before) {
    std::vector<void*> now;
    SnapshotBricks(now);
    for (void* a : now) {
        bool known = false;
        for (void* b : before) known = known || a == b;
        if (!known) return a;
    }
    return nullptr;
}

bool CentreOf(void* actor, ue_wrap::FVector& out) {
    ue_wrap::FVector extent{};
    return actor && (E::GetActorBounds(actor, false, out, extent) || E::TryGetActorLocation(actor, out));
}

void Approach::Sample(void* brick, void* wall) {
    ue_wrap::FVector p{}, c{}, e{};
    if (!brick || !wall || !E::TryGetActorLocation(brick, p) || !E::GetActorBounds(wall, false, c, e)) return;
    const float dx = (std::max)(std::fabs(p.X - c.X) - e.X, 0.f), dy = (std::max)(std::fabs(p.Y - c.Y) - e.Y, 0.f),
                dz = (std::max)(std::fabs(p.Z - c.Z) - e.Z, 0.f);
    last = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (min < 0.f || last < min) min = last;
}

// A brick's own centre this near the wall's box is a touch: about the brick's radius.
bool Approach::Touched() const { return min >= 0.f && min <= 30.f; }

std::string Approach::Say() const {
    if (min < 0.f) return "not sampled";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "closest=%.0f cm last=%.0f cm touched=%d", static_cast<double>(min),
                  static_cast<double>(last), Touched() ? 1 : 0);
    return buf;
}

}  // namespace coop::dev::build_probe::detail
