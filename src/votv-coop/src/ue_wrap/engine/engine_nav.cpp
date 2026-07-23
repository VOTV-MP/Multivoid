// ue_wrap/engine/engine_nav.cpp -- baked-NavMesh navigation + pawn locomotion input.
//
// Engine-wrapper layer (principle 7): thin reflected access to UE4.27's navigation
// system + APawn movement input, holding NO gameplay/network logic. The foundation the
// bot-director (coop/dev/director) drives the possessed player with. Proven at runtime by
// the Phase-0 HALT probe (harness/autotest_navprobe, 2026-07-23): FindPath returns a
// traversable path over the baked NavMesh; AddMovementInput moves the possessed body.
//
// NavMesh calls are STATIC UFunctions on UNavigationSystemV1 -> dispatched on its CDO.
// AddMovementInput is declared on APawn (NOT the leaf mainPlayer_C) -> resolved on the
// Pawn class (FindFunction is exact-owner, does not climb the super chain).

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

void* g_navCdo    = nullptr;   // UNavigationSystemV1 CDO (static dispatch target)
void* g_findPath  = nullptr;   // FindPathToLocationSynchronously
void* g_randReach = nullptr;   // K2_GetRandomReachablePointInRadius
void* g_pawnClass = nullptr;
void* g_addMove   = nullptr;   // APawn::AddMovementInput
void* g_navPathCls = nullptr;
int32_t g_pathPtsOff = -2;     // -2 = uncomputed, -1 = not found

void EnsureNav() {
    if (!g_navCdo)  g_navCdo  = R::FindClassDefaultObject(L"NavigationSystemV1");
    void* cls = R::FindClass(L"NavigationSystemV1");
    if (cls && !g_findPath)  g_findPath  = R::FindFunction(cls, L"FindPathToLocationSynchronously");
    if (cls && !g_randReach) g_randReach = R::FindFunction(cls, L"K2_GetRandomReachablePointInRadius");
}

void EnsurePawn() {
    if (!g_pawnClass) g_pawnClass = R::FindClass(L"Pawn");
    if (g_pawnClass && !g_addMove) g_addMove = R::FindFunction(g_pawnClass, L"AddMovementInput");
}

}  // namespace

bool RandomReachablePoint(void* worldContext, const FVector& origin, float radiusCm, FVector& out) {
    EnsureNav();
    if (!g_navCdo || !g_randReach || !worldContext) return false;
    ParamFrame pf(g_randReach);
    pf.Set<void*>(L"WorldContextObject", worldContext);
    pf.Set<FVector>(L"Origin", origin);
    pf.Set<float>(L"Radius", radiusCm);
    // NavData / FilterClass left null (frame zeroed).
    if (!Call(g_navCdo, pf)) return false;
    if (!pf.Get<bool>(L"ReturnValue")) return false;
    out = pf.Get<FVector>(L"RandomLocation");
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
    constexpr float kBound = 1.0e7f;
    for (int32_t i = 0; i < num; ++i) {
        // TArray<FVector> stride = sizeof(FVector)=12 (FVector is align-4, not a 16-aligned BP struct).
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

}  // namespace ue_wrap::engine
