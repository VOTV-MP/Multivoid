// ue_wrap/engine/engine_nav.cpp -- baked-NavMesh navigation + pawn locomotion input.
//
// Engine-wrapper layer (principle 7): thin reflected access to UE4.27's navigation
// system + APawn movement input, holding NO gameplay/network logic. The foundation the
// bot-director (coop/dev/director) drives the possessed player with. Measured against the
// running game by harness/autotest/autotest_navprobe: FindPath returns a traversable path
// over the baked NavMesh, and AddMovementInput moves the possessed body.
//
// NavMesh calls are STATIC UFunctions on UNavigationSystemV1 -> dispatched on its CDO.
// AddMovementInput is declared on APawn (NOT the leaf mainPlayer_C) -> resolved on the
// Pawn class (FindFunction is exact-owner, does not climb the super chain).

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

void* g_navCdo    = nullptr;   // UNavigationSystemV1 CDO (static dispatch target)
void* g_findPath  = nullptr;   // FindPathToLocationSynchronously
void* g_project   = nullptr;   // K2_ProjectPointToNavigation
void* g_pawnClass = nullptr;
void* g_addMove   = nullptr;   // APawn::AddMovementInput
void* g_navPathCls = nullptr;
int32_t g_pathPtsOff = -2;     // -2 = uncomputed, -1 = not found

void EnsureNav() {
    if (!g_navCdo)  g_navCdo  = R::FindClassDefaultObject(L"NavigationSystemV1");
    if (g_findPath && g_project) return;
    if (void* cls = R::FindClass(L"NavigationSystemV1")) {
        if (!g_findPath) g_findPath = R::FindFunction(cls, L"FindPathToLocationSynchronously");
        if (!g_project) g_project = R::FindFunction(cls, L"K2_ProjectPointToNavigation");
    }
}

void EnsurePawn() {
    if (!g_pawnClass) g_pawnClass = R::FindClass(L"Pawn");
    if (g_pawnClass && !g_addMove) g_addMove = R::FindFunction(g_pawnClass, L"AddMovementInput");
}

}  // namespace

bool ProjectToNav(void* worldContext, const FVector& point, const FVector& extent, FVector* out) {
    EnsureNav();
    if (!g_navCdo || !g_project || !worldContext || !out) return false;
    ParamFrame pf(g_project);
    if (!pf.valid()) return false;
    pf.Set<void*>(L"WorldContextObject", worldContext);
    pf.Set<FVector>(L"Point", point);
    pf.Set<FVector>(L"QueryExtent", extent);   // NavData and FilterClass stay null: the default NavMesh, no filter
    if (!Call(g_navCdo, pf) || !pf.Get<bool>(L"ReturnValue")) return false;
    *out = pf.Get<FVector>(L"ProjectedLocation");
    return true;
}

bool FindNavPath(void* worldContext, const FVector& start, const FVector& end,
                 std::vector<FVector>& outPts) {
    outPts.clear();
    EnsureNav();
    if (!g_navCdo || !g_findPath || !worldContext) return false;
    void* navPath = nullptr;
    {
        ParamFrame pf(g_findPath);
        pf.Set<void*>(L"WorldContextObject", worldContext);
        pf.Set<FVector>(L"PathStart", start);
        pf.Set<FVector>(L"PathEnd", end);
        if (Call(g_navCdo, pf)) navPath = pf.Get<void*>(L"ReturnValue");
    }
    if (!navPath || !R::IsLive(navPath)) return false;
    if (g_pathPtsOff == -2) {
        g_navPathCls = R::ClassOf(navPath);
        g_pathPtsOff = g_navPathCls ? R::FindPropertyOffset(g_navPathCls, L"PathPoints") : -1;
    }
    if (g_pathPtsOff < 0) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(navPath) + g_pathPtsOff;   // TArray<FVector> {Data@0, Num@8}
    void* data = *reinterpret_cast<void**>(base);
    const int32_t num = *reinterpret_cast<int32_t*>(base + 8);
    if (!data || num <= 0 || num > 4096) return false;
    outPts.reserve(static_cast<size_t>(num));
    constexpr float kBound = 1.0e7f;
    for (int32_t i = 0; i < num; ++i) {
        // TArray<FVector> stride = sizeof(FVector)=12 (FVector is align-4, not a 16-aligned BP
        // struct).
        const FVector p = *reinterpret_cast<FVector*>(reinterpret_cast<uint8_t*>(data) + i * 12);
        if (std::fabs(p.X) > kBound || std::fabs(p.Y) > kBound) { outPts.clear(); return false; }
        outPts.push_back(p);
    }
    return outPts.size() >= 2;
}

void AddMovementInput(void* pawn, const FVector& worldDir, float scale, bool force) {
    EnsurePawn();
    if (!g_addMove || !pawn) return;
    ParamFrame pf(g_addMove);
    pf.Set<FVector>(L"WorldDirection", worldDir);
    pf.Set<float>(L"ScaleValue", scale);
    pf.Set<bool>(L"bForce", force);
    Call(pawn, pf);
}

namespace {

// A float member read by its reflected name on the object's own class. False when it is not there.
// The walk is linear; its readers below run once per walk or once per process.
bool ReadFloatMember(void* obj, const wchar_t* name, float* out) {
    if (!obj) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    std::memcpy(out, static_cast<const char*>(obj) + off, sizeof(float));
    return true;
}

}  // namespace

bool ReadWalkLimits(void* character, WalkLimits* out) {
    if (!character || !out) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(character), L"CharacterMovement");
    if (off < 0) return false;
    void* cmc = nullptr;
    std::memcpy(&cmc, static_cast<const char*>(character) + off, sizeof(cmc));
    return cmc && R::IsLive(cmc) && ReadFloatMember(cmc, L"WalkableFloorZ", &out->floorZ) &&
           ReadFloatMember(cmc, L"MaxStepHeight", &out->stepCm);
}

bool ReadNavLimits(NavLimits* out) {
    void* nav = out ? world_singleton::Find(L"RecastNavMesh") : nullptr;
    return nav && ReadFloatMember(nav, L"AgentMaxSlope", &out->maxSlopeDeg) &&
           ReadFloatMember(nav, L"AgentMaxStepHeight", &out->stepCm) &&
           ReadFloatMember(nav, L"AgentRadius", &out->radiusCm);
}

}  // namespace ue_wrap::engine
