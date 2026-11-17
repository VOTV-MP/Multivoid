// coop/dev/drill_ground.cpp -- see coop/dev/drill_ground.h.

#include "coop/dev/drill_ground.h"

#include "coop/dev/level_ground.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace coop::dev::drill_ground {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
using ue_wrap::FVector;

constexpr float kClearUpCm   = 400.f;
constexpr float kClearDownCm = 50.f;
constexpr float kClearTolCm  = 5.f;

bool FootprintClear(void* walker, const FVector& centre, float half, const char* tag) {
    constexpr float kOff[9][2] = {{0, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 9; ++i) {
        const float x = centre.X + kOff[i][0] * half, y = centre.Y + kOff[i][1] * half;
        ue_wrap::trace::Hit h;
        if (!ue_wrap::trace::LineHitStatDyn(walker, FVector{x, y, centre.Z + kClearUpCm},
                                            FVector{x, y, centre.Z - kClearDownCm}, &h) ||
            !h.blocked || std::fabs(h.point.Z - centre.Z) > kClearTolCm) {
            UE_LOGI("[%s] footprint at (%.0f, %.0f) refused: trace %d of 9 %s", tag, centre.X, centre.Y, i,
                    h.blocked ? "struck something other than the floor" : "found nothing");
            return false;
        }
    }
    return true;
}

struct Accept {
    float half, crowdCm;
    const wchar_t* crowdClass;
    const char* tag;
};

bool AcceptSpot(void* ctx, void* walker, const FVector& floorAt) {
    const auto* a = static_cast<const Accept*>(ctx);
    if (!FootprintClear(walker, floorAt, a->half, a->tag)) return false;
    for (void* o : R::FindObjectsByClass(a->crowdClass)) {
        FVector at{};
        if (E::TryGetActorLocation(o, at) && Dist(at, floorAt) < a->crowdCm) {
            UE_LOGI("[%s] spot at (%.0f, %.0f) refused: a %ls is within %.0f cm", a->tag, floorAt.X, floorAt.Y,
                    a->crowdClass, a->crowdCm);
            return false;
        }
    }
    return true;
}

}  // namespace

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool FindSpot(void* walker, const FVector& around, float halfCm, const wchar_t* crowdClass, float crowdCm, const char* tag,
              FVector& spot) {
    Accept a{halfCm, crowdCm, crowdClass, tag};
    return coop::dev::level_ground::FindFrom(walker, around, spot, &AcceptSpot, &a);
}

bool Quiet(void* actor) {
    const PR::VelocityState v = PR::GetPhysicsVelocity(actor, /*linearOnly=*/true);
    return v.ok && std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                             v.linearCmS.Z * v.linearCmS.Z) <= kQuietCmS;
}

void Restart(Settle& s) { s = Settle{}; }

bool Stable(Settle& s, const FVector& at, bool bodyAtRest, int needTicks, const ue_wrap::FRotator* rot) {
    bool turned = false;
    if (rot && s.has) {
        const float p = std::fabs(ue_wrap::NormalizeAxis(rot->Pitch - s.rotAnchor.Pitch));
        const float y = std::fabs(ue_wrap::NormalizeAxis(rot->Yaw - s.rotAnchor.Yaw));
        const float r = std::fabs(ue_wrap::NormalizeAxis(rot->Roll - s.rotAnchor.Roll));
        turned = p > kStableDeg || y > kStableDeg || r > kStableDeg;
    }
    if (!s.has || !bodyAtRest || turned || Dist(at, s.anchor) > kStableCm) {
        s.anchor = at;
        if (rot) s.rotAnchor = *rot;
        s.has = true;
        s.ticks = 0;
        return false;
    }
    return ++s.ticks >= needTicks;
}

void* NearestOf(const wchar_t* cls, const FVector& at, float maxCm) {
    void* best = nullptr;
    float bestD = maxCm;
    for (void* o : R::FindObjectsByClass(cls)) {
        FVector p{};
        if (!E::TryGetActorLocation(o, p)) continue;
        const float d = Dist(p, at);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

}  // namespace coop::dev::drill_ground
