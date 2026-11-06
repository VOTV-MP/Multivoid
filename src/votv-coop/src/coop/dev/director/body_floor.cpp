// coop/dev/director/body_floor.cpp -- see coop/dev/director/body_floor.h.

#include "coop/dev/director/body_floor.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr float kProbeDownCm = 300.f;   // the down-trace from the capsule centre
constexpr float kNearCm      = 300.f;   // the by-name census radius, in 3D

// The object types a character capsule blocks in the engine's default channel order (EObjectTypeQuery): WorldStatic,
// WorldDynamic, Pawn, PhysicsBody, Vehicle, Destructible. A line over the types, not a capsule sweep: ue_wrap::trace
// offers no sweep and no read of the pawn's collision profile, so this is the nearest it has.
constexpr uint8_t kCapsuleTypes[] = {0, 1, 2, 3, 4, 5};

// The words of the classes that could cover a stairwell or hold a body over it.
constexpr const wchar_t* kNearWords[] = {L"hatch", L"trap",    L"floor",   L"cover",  L"slab",
                                         L"plank", L"barrier", L"blocker", L"volume"};

bool ContainsNoCase(std::wstring hay, const wchar_t* needle) {
    for (wchar_t& c : hay) c = static_cast<wchar_t>(std::towlower(c));
    return hay.find(needle) != std::wstring::npos;
}

std::wstring NameText(void* o) { return o ? R::ToString(R::NameOf(o)) : L"?"; }
std::wstring ClassText(void* o) { return o ? R::ClassNameOf(o) : L"?"; }

void SayCurrentFloor(void* player) {
    E::FloorRead f;
    if (!E::ReadCurrentFloor(player, &f)) {
        UE_LOGW("director/diag: stands on: the movement component's CurrentFloor is unread");
        return;
    }
    const std::wstring actor = NameText(f.hit.actor), actorCls = ClassText(f.hit.actor);
    const std::wstring comp = NameText(f.hit.component), compCls = ClassText(f.hit.component);
    UE_LOGW("director/diag: stands on %ls (%ls) / %ls (%ls) at (%.0f,%.0f,%.0f) normal (%.2f,%.2f,%.2f) walkable=%d "
            "floorDist=%.1f blocking=%d", actor.c_str(), actorCls.c_str(), comp.c_str(), compCls.c_str(),
            f.hit.location.X, f.hit.location.Y, f.hit.location.Z, f.hit.normal.X, f.hit.normal.Y, f.hit.normal.Z,
            f.walkable ? 1 : 0, f.floorDist, f.blocking ? 1 : 0);
}

void SayPawnChannelFloor(const PlayerContext& ctx) {
    const ue_wrap::FVector down{ctx.pos.X, ctx.pos.Y, ctx.pos.Z - kProbeDownCm};
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(ctx.player, ctx.pos, down, kCapsuleTypes,
                                           static_cast<int32_t>(sizeof(kCapsuleTypes)), nullptr, 0, &hit)) {
        UE_LOGW("director/diag: pawn-channel floor: the trace is unresolved");
    } else if (!hit.blocked) {
        UE_LOGW("director/diag: pawn-channel floor: clear for %.0f cm", kProbeDownCm);
    } else {
        UE_LOGW("director/diag: pawn-channel floor: %ls/%ls at z=%.0f (the walker's centre z=%.0f)",
                NameText(hit.actor).c_str(), NameText(hit.component).c_str(), hit.point.Z, ctx.pos.Z);
    }
}

void SayActorsNear(const PlayerContext& ctx) {
    void* actorCls = R::FindClass(L"Actor");
    int found = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; actorCls && i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        const std::wstring cls = R::ClassNameOf(o);
        bool named = false;
        for (const wchar_t* w : kNearWords) named = named || ContainsNoCase(cls, w);
        if (!named || !R::IsDescendantOfAny(R::ClassOf(o), &actorCls, 1)) continue;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(o, at)) continue;
        const float dx = at.X - ctx.pos.X, dy = at.Y - ctx.pos.Y, dz = at.Z - ctx.pos.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > kNearCm) continue;
        UE_LOGW("director/diag: near %ls %ls at (%.0f,%.0f,%.0f) dist=%.0f", cls.c_str(), NameText(o).c_str(), at.X,
                at.Y, at.Z, dist);
        ++found;
    }
    UE_LOGW("director/diag: %d named actors within %.0f cm", found, kNearCm);
}

}  // namespace

void SayWhatBodyStandsOn(const PlayerContext& ctx) {
    SayCurrentFloor(ctx.player);
    SayPawnChannelFloor(ctx);
    SayActorsNear(ctx);
}

}  // namespace coop::director
