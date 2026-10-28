// coop/dev/octa_drill_births.cpp -- the births arm of the Octa drill: the host calls the gamemode's own spawn verb with the
// spawn menu's Octa row and must enrol the Octa; the client calls the night roll's event, which the spawn authority
// refuses, and must materialise the Octa's mirror. See coop/dev/octa_drill.h and coop/dev/octa_drill_internal.h.

#include "coop/dev/octa_drill.h"

#include "coop/dev/director/routes.h"
#include "octa_drill_internal.h"  // co-located private header

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <vector>

namespace coop::dev::octa_drill::detail {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

namespace {

// The host's player and the first standpoint on a ring about it that a route reaches; false once the arm has ended.
bool RingPoint(void* me, FVector& out) {
    FVector at{};
    if (!E::TryGetActorLocation(me, at)) return false;
    const std::vector<FVector> ring = D::ReachableStandpoints(me, at, kSpawnRingCm, kStandpoints, kStandReachCm);
    if (ring.empty()) {
        Fail("no NavMesh route from the host reaches a standpoint 1500 cm away, so the Octa has no ground to stand on");
        return false;
    }
    out = ring[0];
    return true;
}

}  // namespace

// births: the gamemode's own spawn verb with the spawn menu's Octa row, the call the night roll shares.
bool CallGamemodeSpawn(const FVector& at) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"spawnPropThroughGamemode") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName row = ue_wrap::fname_utils::StringToFName(L"cube_0");
    ue_wrap::FTransform xform{};
    xform.TX = at.X;
    xform.TY = at.Y;
    xform.TZ = at.Z;
    return f.SetRaw(L"prop", &row, sizeof(row)) && f.SetRaw(L"InputPin", &xform, sizeof(xform)) &&
           f.Set<int32_t>(L"amount", 1) && ue_wrap::Call(gm, f);
}

void HostBirths(void* me) {
    FVector at{};
    if (!RingPoint(me, at)) return;
    if (!CallGamemodeSpawn(at)) {
        Fail("the gamemode's spawnPropThroughGamemode did not run");
        return;
    }
    UE_LOGI("[OCTA-DRILL] host: births called spawnPropThroughGamemode(cube_0) at=%.0f,%.0f,%.0f", at.X, at.Y, at.Z);
    g_hstep = HStep::Watch;
}

void HostWatchBirths() {
    void* octa = FirstInstance(kOctaClass);
    const uint32_t eid = NpcEidOf(octa);
    if (eid) {
        UE_LOGI("[OCTA-DRILL] births PASS role=host eid=%u t=%llu", eid, Now());
        g_hstep = HStep::Done;
        return;
    }
    if (Expired(kBirthsBoundMs)) Fail("births host: the Octa the gamemode spawned was never enrolled as an NPC element");
}

bool ClientBirthsCall() {
    // The night roll's event, which the spawn authority refuses on a client: the call, and the line it logs.
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"CustomEvent_5") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(gm, f)) {
        Fail("the gamemode's CustomEvent_5 did not run");
        return false;
    }
    g_calledEvent = true;
    UE_LOGI("[OCTA-DRILL] births client: called CustomEvent_5");
    return true;
}

void ClientBirthsFound(uint32_t eid) {
    UE_LOGI("[OCTA-DRILL] births PASS role=client eid=%u t=%llu", eid, Now());
    Go(CStep::Done);
    Unguard();
}

}  // namespace coop::dev::octa_drill::detail
