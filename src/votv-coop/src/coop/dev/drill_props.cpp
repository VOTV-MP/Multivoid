// coop/dev/drill_props.cpp -- see coop/dev/drill_props.h.

#include "coop/dev/drill_props.h"

#include "coop/dev/spawn_npc.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <string>

namespace coop::dev::drill_props {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool FloorAt(void* ctx, float x, float y, float fromZ, float downCm, float minColumnCm, float* floorZ,
             FloorMiss* miss) {
    const uint8_t types[2] = {0, 1};
    void* ignore[coop::players::kMaxPeers];
    const int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ue_wrap::trace::Hit hit;
    FloorMiss why = FloorMiss::None;
    if (!ue_wrap::trace::LineHitForObjects(ctx, {x, y, fromZ}, {x, y, fromZ - downCm}, types, 2, ignore, nIgnore, &hit))
        why = FloorMiss::TraceFailed;
    else if (!hit.blocked)
        why = FloorMiss::NoFloor;
    else if (fromZ - hit.point.Z < minColumnCm)
        why = FloorMiss::InsideGeometry;
    if (miss) *miss = why;
    if (why != FloorMiss::None) return false;
    *floorZ = hit.point.Z;
    return true;
}

Drop DropOnFloor(void* ctx, void* cls, const wchar_t* npc, float x, float y, float fromZ, float downCm,
                 float minColumnCm, float riseCm, void** out, float* floorZ) {
    float found = 0.f;
    if (!FloorAt(ctx, x, y, fromZ, downCm, minColumnCm, &found)) return Drop::NoFloor;
    const ue_wrap::FVector at{x, y, found + riseCm};
    void* const p = cls ? E::SpawnActor(cls, at) : coop::dev::spawn_npc::SpawnAt(npc, at);
    if (!p) return Drop::SpawnFailed;
    *out = p;
    if (floorZ) *floorZ = found;
    return Drop::Spawned;
}

bool TurnOffKerfur(void* npc) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(npc), L"dropKerfurProp");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(npc, f);
}

bool HasKey(void* actor) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return !key.empty() && key != L"None";
}

}  // namespace coop::dev::drill_props
