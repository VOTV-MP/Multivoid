// harness/autotest_navprobe.cpp -- the Phase-0 HALT probe for the autonomous
// bot-director (design: research/findings/tooling/votv-baritone-analog-autonomous-
// director-DESIGN-2026-07-23.md, section B7). SOLO, role-agnostic -- no connection
// needed; it only needs a possessed local player in the gameplay world.
//
// WHY THIS EXISTS: the director design is HALT-gated on two independent, must-
// measure-before-build gates (design B7 "Phase 0"). Nothing of the director is
// built until BOTH are measured on a real running game, because each is a load-
// bearing INFERENCE the static RE could not settle:
//
//   GATE A -- does UNavigationSystemV1::FindPathToLocationSynchronously RETURN a
//     traversable path over VOTV's baked NavMesh? The cooked umaps EXPORT the nav
//     actors (RecastNavMesh x49 etc., measured), but "actors authored" != "navmesh
//     BUILT with traversable polys" -- a runtime fact only this probe settles.
//     Discriminated three ways (probe-must-count): call-failed (fn unresolved /
//     bad world context / null return) vs no-route (path returned but <=1 point /
//     invalid) vs traversable (>1 points + IsValid). We pick the ENDPOINT via
//     K2_GetRandomReachablePointInRadius so it is GUARANTEED reachable by
//     construction -- removing the "I chose an unreachable target" confound (a
//     failed FindPath to a random-reachable point is then a real navmesh signal,
//     not a bad-target artifact). That query ALSO doubly-confirms the navmesh: it
//     only succeeds if the navmesh has reachable polygons near the player.
//
//   GATE B -- does a REFLECTED APawn::AddMovementInput actually MOVE the possessed
//     body? This is a dispatch-visibility question (does the native exec thunk run
//     the CharacterMovement input path when we call it via ProcessEvent). Measured
//     by sweeping 4 world directions (so a wall in one direction is not a false
//     negative) and reading the HORIZONTAL displacement (X/Y only -- Z would fold
//     in gravity/falling, not walk input).
//
// TRAP AVOIDED (findfunction-does-not-walk-the-superclass-chain): AddMovementInput
// is declared on APawn, NOT on mainPlayer_C. R::FindFunction is exact-owner (it does
// NOT climb to the super), so we resolve it on FindClass(L"Pawn") -- resolving it on
// the leaf class would return nullptr = a silent permanent no-op. Same reason the
// static nav UFunctions are dispatched on the NavigationSystemV1 CDO.
//
// The two gates are NECESSARY-not-sufficient: they gate the ATTEMPT. Closed-loop
// convergence (steer -> arrive) is proven only by the Phase-1 flagship RUN, which
// fails safe. This probe just picks the fallback rung (design B7): A&B -> rung0 real
// walk; A-only -> rung1 swept SetActorLocation along the real FindPath; A-fail ->
// rung2 standoff-teleport (today's chippile, a P0 failure). NON-DESTRUCTIVE: the
// player is teleported back to its start location at the end.
//
// Gated by env VOTVCOOP_RUN_NAV_PROBE="1". Launch: solo (mp.py) or in a peer.
// Greppable verdict: "nav_probe: VERDICT".

#include "harness/autotest.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// Run a game-thread closure and block until it stores into `done` (1=ok, 2=fail).
// UObject state is game-thread only (the harness pattern, see autotest_chippile).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// Class + function names (kept local to the probe; not promoted to sdk_profile
// until the director itself is built at N=3 -- fix-then-generalize).
constexpr const wchar_t* kNavSysClass   = L"NavigationSystemV1";
constexpr const wchar_t* kNavPathClass  = L"NavigationPath";
constexpr const wchar_t* kFindPathFn    = L"FindPathToLocationSynchronously";
constexpr const wchar_t* kRandReachFn   = L"K2_GetRandomReachablePointInRadius";
constexpr const wchar_t* kPathIsValidFn = L"IsValid";
constexpr const wchar_t* kPathLenFn     = L"GetPathLength";
constexpr const wchar_t* kPawnClass     = L"Pawn";
constexpr const wchar_t* kAddMoveFn     = L"AddMovementInput";

}  // namespace

void RunNavHaltProbe() {
    UE_LOGI("nav_probe: Phase-0 HALT probe starting (Gate A = FindPath returns a path; "
            "Gate B = reflected AddMovementInput moves the body). SOLO, role-agnostic; "
            "waiting for a possessed local player in the gameplay world.");

    // ---- 0. SETTLE: wait for a possessed local player (naturally gates on being in
    //         gameplay, not the menu -- no mainPlayer_C is possessed in the menu).
    void* player = nullptr;
    {
        const int kWaitCapS = 60;
        int waitedS = 0;
        while (waitedS < kWaitCapS) {
            struct S { void* p = nullptr; };
            auto s = std::make_shared<S>();
            const int r = RunGT([s](std::atomic<int>& d) {
                void* p = coop::players::Registry::Get().Local();
                // GetController()!=null is THE definitive possessed-local discriminator.
                if (p && R::IsLive(p) && E::GetController(p)) { s->p = p; d.store(1); }
                else d.store(2);
            });
            if (r == 1) { player = s->p; break; }
            ::Sleep(1000);
            ++waitedS;
        }
        if (!player) {
            UE_LOGW("nav_probe: no possessed local player after %d s -- aborting (not in gameplay?)", kWaitCapS);
            return;
        }
    }
    UE_LOGI("nav_probe: possessed local player=%p live -- running the two gates", player);

    // =====================================================================
    // GATE A -- FindPath returns a traversable path over the baked NavMesh.
    // =====================================================================
    struct GateA {
        bool  fnResolved   = false;   // the FindPath + random-reachable UFunctions resolved
        bool  reachOk      = false;   // K2_GetRandomReachablePointInRadius succeeded (navmesh queryable)
        bool  pathReturned = false;   // FindPath returned a non-null UNavigationPath
        bool  pathValid    = false;   // UNavigationPath::IsValid()
        int32_t points     = 0;       // PathPoints.Num
        float pathLen      = -1.f;    // GetPathLength()
        ue_wrap::FVector start{}, end{};
    };
    auto ga = std::make_shared<GateA>();
    RunGT([player, ga](std::atomic<int>& d) {
        void* navCdo   = R::FindClassDefaultObject(kNavSysClass);  // static UFunctions dispatch on the CDO
        void* navCls   = R::FindClass(kNavSysClass);
        void* findFn   = navCls ? R::FindFunction(navCls, kFindPathFn)  : nullptr;
        void* randFn   = navCls ? R::FindFunction(navCls, kRandReachFn) : nullptr;
        ga->fnResolved = (navCdo && findFn && randFn);
        if (!ga->fnResolved) {
            UE_LOGW("nav_probe: GATE A -- unresolved (navCdo=%p findFn=%p randFn=%p); the nav UFunctions "
                    "are not where expected -- Gate A CALL-FAILED", navCdo, findFn, randFn);
            d.store(1); return;
        }
        ga->start = E::GetActorLocation(player);

        // A.1 -- a GUARANTEED-reachable endpoint (removes the unreachable-target confound).
        {
            ue_wrap::ParamFrame pf(randFn);
            pf.Set<void*>(L"WorldContextObject", player);
            pf.Set<ue_wrap::FVector>(L"Origin", ga->start);
            pf.Set<float>(L"Radius", 1500.f);          // 15 m -- comfortably inside a room
            // NavData / FilterClass left null (frame is zeroed).
            if (ue_wrap::Call(navCdo, pf)) {
                ga->reachOk = pf.Get<bool>(L"ReturnValue");
                ga->end     = pf.Get<ue_wrap::FVector>(L"RandomLocation");
            }
        }
        if (!ga->reachOk) {
            // No reachable poly near the player -> strong signal the navmesh is not built
            // near here. Fall back to a fixed 5 m +X offset so FindPath still gets ATTEMPTED
            // (discriminates "no navmesh at all" from "this exact spot has no reachable poly").
            ga->end = ue_wrap::FVector{ ga->start.X + 500.f, ga->start.Y, ga->start.Z };
            UE_LOGW("nav_probe: GATE A -- K2_GetRandomReachablePointInRadius FAILED near the player "
                    "(navmesh likely not built here); trying FindPath to a fixed +5m offset anyway");
        }

        // A.2 -- FindPathToLocationSynchronously(player, start, end).
        void* navPath = nullptr;
        {
            ue_wrap::ParamFrame pf(findFn);
            pf.Set<void*>(L"WorldContextObject", player);
            pf.Set<ue_wrap::FVector>(L"PathStart", ga->start);
            pf.Set<ue_wrap::FVector>(L"PathEnd", ga->end);
            // PathfindingContext / FilterClass left null.
            if (ue_wrap::Call(navCdo, pf))
                navPath = pf.Get<void*>(L"ReturnValue");
        }
        ga->pathReturned = (navPath != nullptr);
        if (navPath && R::IsLive(navPath)) {
            // A.3 -- read PathPoints.Num (TArray {ptr@0, Num@8, Max@12}) via the reflected offset.
            void* pathCls = R::ClassOf(navPath);
            const int32_t off = pathCls ? R::FindPropertyOffset(pathCls, L"PathPoints") : -1;
            if (off >= 0)
                ga->points = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(navPath) + off + 8);
            // A.4 -- IsValid() + GetPathLength() (corroboration).
            if (void* vfn = pathCls ? R::FindFunction(pathCls, kPathIsValidFn) : nullptr) {
                ue_wrap::ParamFrame pf(vfn);
                if (ue_wrap::Call(navPath, pf)) ga->pathValid = pf.Get<bool>(L"ReturnValue");
            }
            if (void* lfn = pathCls ? R::FindFunction(pathCls, kPathLenFn) : nullptr) {
                ue_wrap::ParamFrame pf(lfn);
                if (ue_wrap::Call(navPath, pf)) ga->pathLen = pf.Get<float>(L"ReturnValue");
            }
        }
        d.store(1);
    });
    const bool gateA = ga->fnResolved && ga->reachOk && ga->pathReturned && ga->pathValid && ga->points > 1;
    UE_LOGI("nav_probe: GATE A %s -- fnResolved=%d reachOk=%d start=(%.0f,%.0f,%.0f) end=(%.0f,%.0f,%.0f) "
            "pathReturned=%d IsValid=%d points=%d len=%.0fcm",
            gateA ? "PASS" : "FAIL", ga->fnResolved ? 1 : 0, ga->reachOk ? 1 : 0,
            ga->start.X, ga->start.Y, ga->start.Z, ga->end.X, ga->end.Y, ga->end.Z,
            ga->pathReturned ? 1 : 0, ga->pathValid ? 1 : 0, ga->points, ga->pathLen);

    // =====================================================================
    // GATE B -- reflected AddMovementInput moves the possessed body.
    // Sweep 4 world directions (wall-robust); measure HORIZONTAL displacement.
    // =====================================================================
    struct GateB { void* fn = nullptr; float best = 0.f; float d0 = 0, d1 = 0, d2 = 0, d3 = 0; ue_wrap::FVector home{}; };
    auto gb = std::make_shared<GateB>();
    // Resolve AddMovementInput on the DECLARING class (APawn), NOT the leaf.
    RunGT([gb, player](std::atomic<int>& d) {
        void* pawnCls = R::FindClass(kPawnClass);
        gb->fn   = pawnCls ? R::FindFunction(pawnCls, kAddMoveFn) : nullptr;
        gb->home = E::GetActorLocation(player);
        d.store(1);
    });
    if (!gb->fn) {
        UE_LOGW("nav_probe: GATE B -- AddMovementInput not found on the Pawn class -- CALL-FAILED "
                "(check the declaring class / SDK name)");
    } else {
        const ue_wrap::FVector dirs[4] = {
            { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f },
        };
        float deltas[4] = { 0, 0, 0, 0 };
        for (int di = 0; di < 4; ++di) {
            ue_wrap::FVector loc0{};
            RunGT([player, &loc0](std::atomic<int>& dd) { loc0 = E::GetActorLocation(player); dd.store(1); });
            // ~1 s of per-frame input: AddMovementInput must be re-issued each frame
            // (the pawn consumes + clears ControlInputVector on its movement tick).
            const ue_wrap::FVector dir = dirs[di];
            for (int k = 0; k < 40; ++k) {
                RunGT([gb, player, dir](std::atomic<int>& dd) {
                    ue_wrap::ParamFrame pf(gb->fn);
                    pf.Set<ue_wrap::FVector>(L"WorldDirection", dir);
                    pf.Set<float>(L"ScaleValue", 1.0f);
                    pf.Set<bool>(L"bForce", true);      // force even if input is nominally disabled
                    ue_wrap::Call(player, pf);
                    dd.store(1);
                });
                ::Sleep(25);
            }
            ue_wrap::FVector loc1{};
            RunGT([player, &loc1](std::atomic<int>& dd) { loc1 = E::GetActorLocation(player); dd.store(1); });
            deltas[di] = HorizDist(loc1, loc0);
            if (deltas[di] > gb->best) gb->best = deltas[di];
            UE_LOGI("nav_probe: GATE B -- dir(%.0f,%.0f) horizontal move = %.0f cm", dir.X, dir.Y, deltas[di]);
            // Return toward home between directions so the sweep doesn't wander far.
            RunGT([gb, player](std::atomic<int>& dd) { E::SetActorLocation(player, gb->home); dd.store(1); });
            ::Sleep(200);
        }
        gb->d0 = deltas[0]; gb->d1 = deltas[1]; gb->d2 = deltas[2]; gb->d3 = deltas[3];
        // Restore the start location exactly (non-destructive probe).
        RunGT([gb, player](std::atomic<int>& dd) { E::SetActorLocation(player, gb->home); dd.store(1); });
    }
    const float kMoveThreshCm = 30.f;     // any direction moving >30 cm in ~1 s = the input drives movement
    const bool gateB = gb->fn && gb->best > kMoveThreshCm;
    UE_LOGI("nav_probe: GATE B %s -- fnResolved=%d best=%.0f cm (thresh=%.0f) per-dir=[%.0f,%.0f,%.0f,%.0f]",
            gateB ? "PASS" : "FAIL", gb->fn ? 1 : 0, gb->best, kMoveThreshCm, gb->d0, gb->d1, gb->d2, gb->d3);

    // ---- VERDICT + the fallback rung it picks (design B7).
    const char* rung =
        (gateA && gateB) ? "rung0 (real walk: FindPath + per-tick AddMovementInput) -- the Phase-1 walked-grab flagship is UNBLOCKED"
      : (gateA && !gateB) ? "rung1 (swept SetActorLocation along the real FindPath -- real path traversal, no movement physics; LEGITIMATE only for scenarios with NO movement/pose/velocity invariant)"
      : (!gateA) ? "rung2 (standoff-teleport = today's chippile; P0 movement FAILED -- NOT a flagship). Escalate: IDA the nav build / FindPath call site."
      : "rung?";
    UE_LOGI("nav_probe: VERDICT -- GATE A(FindPath)=%s GATE B(AddMovementInput)=%s -> %s",
            gateA ? "PASS" : "FAIL", gateB ? "PASS" : "FAIL", rung);
}

DWORD WINAPI NavHaltProbeThread(LPVOID /*arg*/) {
    RunNavHaltProbe();
    return 0;
}

}  // namespace harness::autotest
