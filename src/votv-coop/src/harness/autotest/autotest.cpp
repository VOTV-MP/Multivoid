// harness/autotest.cpp -- Autonomous grab test (no user E-press required).
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).
// See harness/autotest.h for the public interface.
//
// Expected log lines (on host) after this runs:
//   grab_hook[PHC.Grab]       (once, at GrabComponentAtLocation call)
//   grab_hook[PHC.SetTarget]  (once per second, throttled to 1-in-30)
//   grab_hook[PHC.Release PRE](once, at ReleaseComponent call -- logs the
//                              GrabbedComponent we read off the handle)
//
// Pieces this verifies:
//   1. FindClass + FindFunction resolution for engine-native UFunctions
//   2. ProcessEvent dispatch path through reflection::CallFunction
//   3. Our observer detour catches engine-native UFunctions (not just BP)
//   4. The +176 GrabbedComponent field read in PHC.Release PRE
//
// What this does NOT verify (still requires hands-on or further code):
//   - VOTV BP-graph reaction to the forced grab (the BP doesn't know we
//     called the native UFunction so mainPlayer.grabbing_actor stays null)
//   - Heavy drag path (UPhysicsConstraintComponent) -- different setup
//   - Receiver-side reception (no wire shipped yet -- Stage 4 blocker)
//
// Pass-2 verification: when run on BOTH peers (host AND client), each does
// a prop scan; comparing their nearest-prop Key.ComparisonIndex tells us
// whether cooked-content FNames are stable cross-peer (which is the
// hypothesis Stage 4's wire serialization depends on). Only the HOST does
// the actual grab/move/release calls -- client is scan-only.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "harness/screenshot.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

}  // namespace

void RunAutonomousGrabTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");  // default Host if unset
    const char* roleStr = isHost ? "host" : "client";
    UE_LOGI("grab_test: starting autonomous routine on %s (waiting 10 s for stabilization)", roleStr);
    ::Sleep(10000);

    // ---- 1. Resolve actors + components (game thread).
    struct Resolved {
        void* player = nullptr;
        void* grabHandle = nullptr;
        void* propBase = nullptr;
        void* phcCls = nullptr;
        void* grabFn = nullptr;
        void* setTargetFn = nullptr;
        void* releaseFn = nullptr;
        int32_t grabFrameSize = 0;
        int32_t grabPComp = -1, grabPBone = -1, grabPLoc = -1;
        int32_t stFrameSize = 0;
        int32_t stPLoc = -1;
        bool ok = false;
    };
    auto rsv = std::make_shared<Resolved>();
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([rsv, done] {
        rsv->player = R::FindObjectByClass(P::name::MainPlayerClass);
        if (!rsv->player) { UE_LOGW("grab_test: mainPlayer not found"); done->store(2); return; }
        rsv->grabHandle = ue_wrap::engine::ReadMainPlayerGrabHandle(rsv->player);
        if (!rsv->grabHandle) { UE_LOGW("grab_test: mainPlayer.grabHandle is null"); done->store(2); return; }
        rsv->propBase = R::FindClass(P::name::PropClass);
        if (!rsv->propBase) { UE_LOGW("grab_test: prop_C UClass not found"); done->store(2); return; }
        rsv->phcCls = R::FindClass(P::name::PhysicsHandleComponentClass);
        rsv->grabFn      = R::FindFunction(rsv->phcCls, P::name::GrabComponentAtLocationFn);
        rsv->setTargetFn = R::FindFunction(rsv->phcCls, P::name::SetTargetLocationFn);
        rsv->releaseFn   = R::FindFunction(rsv->phcCls, P::name::ReleaseComponentFn);
        if (!rsv->grabFn || !rsv->setTargetFn || !rsv->releaseFn) {
            UE_LOGW("grab_test: PHC UFunction lookup failed (grab=%p set=%p rel=%p)",
                    rsv->grabFn, rsv->setTargetFn, rsv->releaseFn);
            done->store(2); return;
        }
        rsv->grabFrameSize = R::FunctionFrameSize(rsv->grabFn);
        rsv->grabPComp = R::FindParamOffset(rsv->grabFn, L"Component");
        rsv->grabPBone = R::FindParamOffset(rsv->grabFn, L"InBoneName");
        rsv->grabPLoc  = R::FindParamOffset(rsv->grabFn, L"GrabLocation");
        rsv->stFrameSize = R::FunctionFrameSize(rsv->setTargetFn);
        rsv->stPLoc = R::FindParamOffset(rsv->setTargetFn, L"NewLocation");
        UE_LOGI("grab_test: resolve OK player=%p grabHandle=%p propBase=%p "
                "grabFn=%p frame=%d (Comp@%d Bone@%d Loc@%d) "
                "setTargetFn=%p frame=%d (Loc@%d) releaseFn=%p",
                rsv->player, rsv->grabHandle, rsv->propBase,
                rsv->grabFn, rsv->grabFrameSize, rsv->grabPComp, rsv->grabPBone, rsv->grabPLoc,
                rsv->setTargetFn, rsv->stFrameSize, rsv->stPLoc, rsv->releaseFn);
        rsv->ok = (rsv->grabPComp >= 0 && rsv->grabPBone >= 0 && rsv->grabPLoc >= 0 && rsv->stPLoc >= 0);
        done->store(rsv->ok ? 1 : 2);
    });
    while (done->load() == 0) ::Sleep(5);
    if (!rsv->ok) { UE_LOGW("grab_test: resolve failed -- aborting"); return; }

    // ---- 2. Find nearest Aprop_C derivative (super-walk over GUObjectArray).
    // Anchor: env VOTVCOOP_GRAB_TEST_ANCHOR_{X,Y,Z} if set, else the local
    // player's world location. The env-anchor lets BOTH peers scan around
    // the SAME world point.
    const std::string anchXs = cfg::ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_X");
    const std::string anchYs = cfg::ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_Y");
    const std::string anchZs = cfg::ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_Z");
    const bool haveAnchor = !anchXs.empty() && !anchYs.empty() && !anchZs.empty();
    const ue_wrap::FVector envAnchor{
        haveAnchor ? static_cast<float>(std::atof(anchXs.c_str())) : 0.f,
        haveAnchor ? static_cast<float>(std::atof(anchYs.c_str())) : 0.f,
        haveAnchor ? static_cast<float>(std::atof(anchZs.c_str())) : 0.f};
    struct PropResult {
        void* prop = nullptr; void* mesh = nullptr; float dist = 0.f; std::wstring cls;
        void* heavyProp = nullptr; void* heavyMesh = nullptr; float heavyDist = 0.f; std::wstring heavyCls;
        int totalHeavy = 0;
    };
    auto pr = std::make_shared<PropResult>();
    done->store(0);
    GT::Post([rsv, pr, done, haveAnchor, envAnchor] {
        const ue_wrap::FVector pLoc = haveAnchor ? envAnchor
                                                : ue_wrap::engine::GetActorLocation(rsv->player);
        UE_LOGI("grab_test: scan anchor = (%.0f, %.0f, %.0f) [%s]",
                pLoc.X, pLoc.Y, pLoc.Z, haveAnchor ? "env" : "player");
        ue_wrap::prop::ScanStats stats;
        const auto best = ue_wrap::prop::FindNearest(pLoc, /*wantHeavy=*/false, &stats);
        pr->prop = best.prop;
        pr->mesh = best.mesh;
        pr->dist = best.dist;
        pr->cls  = best.className;
        pr->totalHeavy = stats.totalHeavy;
        UE_LOGI("grab_test: prop scan scanned=%d candidates=%d totalHeavy=%d "
                "nearest=%p (%ls) dist=%.1f mesh=%p",
                stats.totalScanned, stats.candidates, stats.totalHeavy,
                pr->prop, pr->cls.c_str(), pr->dist, pr->mesh);
        if (pr->prop) {
            const auto key = ue_wrap::prop::GetKey(pr->prop);
            UE_LOGI("grab_test: prop fields -- heavy=%d Static=%d frozen=%d Key='%ls' (idx=%d num=%d)",
                    best.heavy ? 1 : 0, best.isStatic ? 1 : 0, best.isFrozen ? 1 : 0,
                    best.keyString.c_str(), key.ComparisonIndex, key.Number);
        }
        const auto bestHeavy = ue_wrap::prop::FindNearest(pLoc, /*wantHeavy=*/true);
        pr->heavyProp = bestHeavy.prop;
        pr->heavyMesh = bestHeavy.mesh;
        pr->heavyDist = bestHeavy.dist;
        pr->heavyCls  = bestHeavy.className;
        if (pr->heavyProp) {
            UE_LOGI("grab_test: nearest HEAVY=%p (%ls) dist=%.1f mesh=%p",
                    pr->heavyProp, pr->heavyCls.c_str(), pr->heavyDist, pr->heavyMesh);
        } else {
            UE_LOGI("grab_test: NO heavy props in scene (totalHeavy=0) -- skip heavy-grab arm");
        }
        done->store(pr->prop && pr->mesh ? 1 : 2);
    });
    while (done->load() == 0) ::Sleep(5);
    if (done->load() != 1) { UE_LOGW("grab_test: no suitable prop found -- aborting"); return; }

    // CLIENT: scan + log only. Don't drive any UFunctions.
    if (!isHost) {
        UE_LOGI("grab_test: CLIENT scan-only complete -- compare prop fields above with HOST log");
        UE_LOGI("grab_test: DONE (client scan-only)");
        return;
    }

    // ---- 3. (HOST) Compute hand position.
    auto handPos = std::make_shared<ue_wrap::FVector>();
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        ue_wrap::FVector pLoc = ue_wrap::engine::GetActorLocation(rsv->player);
        ue_wrap::FVector fwd  = ue_wrap::engine::GetActorForwardVector(rsv->player);
        handPos->X = pLoc.X + fwd.X * 60.f;
        handPos->Y = pLoc.Y + fwd.Y * 60.f;
        handPos->Z = pLoc.Z + 80.f;
        ue_wrap::engine::SetActorLocation(pr->prop, *handPos);
        UE_LOGI("grab_test: teleported prop -> (%.1f, %.1f, %.1f) (camera-level, in frame)",
                handPos->X, handPos->Y, handPos->Z);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 4. Call grabHandle.GrabComponentAtLocation + write BP state.
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        std::vector<uint8_t> frame(static_cast<size_t>(rsv->grabFrameSize), 0);
        *reinterpret_cast<void**>(frame.data() + rsv->grabPComp) = pr->mesh;
        *reinterpret_cast<R::FName*>(frame.data() + rsv->grabPBone) = R::FName{0, 0};
        *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->grabPLoc) = *handPos;
        const bool ok = R::CallFunction(rsv->grabHandle, rsv->grabFn, frame.data());
        ue_wrap::engine::WriteMainPlayerGrabbingPair(rsv->player, pr->prop, pr->mesh);
        UE_LOGI("grab_test: CallFunction(GrabComponentAtLocation) -> %d + wrote mainPlayer.grabbing_{actor,component}", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 5. Hold + tilt camera over 15 sec (2 screenshots).
    UE_LOGI("grab_test: holding + tilting camera up over 15 sec (2 screenshots: before-tilt vs after-tilt)");
    auto driveTick = [&](float pitchDelta) {
        done->store(0);
        GT::Post([rsv, pitchDelta, done] {
            void* ctrl = ue_wrap::engine::GetController(rsv->player);
            if (ctrl && pitchDelta != 0.f) {
                ue_wrap::FRotator rot = ue_wrap::engine::GetControlRotation(ctrl);
                rot.Pitch = ue_wrap::NormalizeAxis(rot.Pitch);
                rot.Pitch += pitchDelta;
                if (rot.Pitch >  60.f) rot.Pitch =  60.f;
                if (rot.Pitch < -60.f) rot.Pitch = -60.f;
                ue_wrap::engine::SetControlRotation(ctrl, rot);
            }
            const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
            const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
            const float kDeg2Rad = 3.14159265358979323846f / 180.f;
            const float py = camRot.Pitch * kDeg2Rad;
            const float yw = camRot.Yaw   * kDeg2Rad;
            const float cp = std::cos(py), sp = std::sin(py);
            const float cy = std::cos(yw), sy = std::sin(yw);
            ue_wrap::FVector fwd{cp * cy, cp * sy, sp};
            ue_wrap::FVector target{camLoc.X + fwd.X * 60.f,
                                    camLoc.Y + fwd.Y * 60.f,
                                    camLoc.Z + fwd.Z * 60.f};
            std::vector<uint8_t> frame(static_cast<size_t>(rsv->stFrameSize), 0);
            *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->stPLoc) = target;
            R::CallFunction(rsv->grabHandle, rsv->setTargetFn, frame.data());
            done->store(1);
        });
        while (done->load() == 0) ::Sleep(5);
    };

    for (int i = 0; i < 30; ++i) {
        ::Sleep(500);
        const float pitchDelta = (i >= 8 && i < 20) ? 3.f : 0.f;
        driveTick(pitchDelta);
        if (i == 6) {
            const bool ok = harness::screenshot::Capture(L"grab-before-tilt");
            UE_LOGI("grab_test: HOST SCREENSHOT 1 (before tilt, level camera) saved=%d", ok);
        }
        if (i == 26) {
            const bool ok = harness::screenshot::Capture(L"grab-after-tilt");
            UE_LOGI("grab_test: HOST SCREENSHOT 2 (after +36deg pitch tilt) saved=%d", ok);
        }
    }

    // ---- 6. Release + Throw arm (atomic in one game-thread tick).
    done->store(0);
    GT::Post([rsv, pr, done] {
        void* primCls = R::FindClass(P::name::PrimitiveComponentClass);
        void* addImpulseFn = R::FindFunction(primCls, P::name::AddImpulseFn);
        if (!addImpulseFn) { UE_LOGW("grab_test: AddImpulse UFunction not found"); done->store(2); return; }
        const int32_t frameSize = R::FunctionFrameSize(addImpulseFn);
        const int32_t pImp  = R::FindParamOffset(addImpulseFn, L"impulse");
        const int32_t pBone = R::FindParamOffset(addImpulseFn, L"BoneName");
        const int32_t pVel  = R::FindParamOffset(addImpulseFn, L"bVelChange");
        if (pImp < 0 || pBone < 0 || pVel < 0) {
            UE_LOGW("grab_test: AddImpulse param offsets missing (imp=%d bone=%d vel=%d)", pImp, pBone, pVel);
            done->store(2); return;
        }
        std::vector<uint8_t> frame(static_cast<size_t>(frameSize), 0);
        *reinterpret_cast<ue_wrap::FVector*>(frame.data() + pImp) = ue_wrap::FVector{0.f, 0.f, 500.f};
        *reinterpret_cast<R::FName*>(frame.data() + pBone) = R::FName{0, 0};
        *reinterpret_cast<bool*>(frame.data() + pVel) = false;
        const bool impOk = R::CallFunction(pr->mesh, addImpulseFn, frame.data());
        const bool relOk = R::CallFunction(rsv->grabHandle, rsv->releaseFn, nullptr);
        ue_wrap::engine::WriteMainPlayerGrabbingPair(rsv->player, nullptr, nullptr);
        UE_LOGI("grab_test: AddImpulse=%d Release=%d (atomic in same game-thread tick)",
                impOk, relOk);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    ::Sleep(700);
    {
        const bool ok = harness::screenshot::Capture(L"grab-post-throw");
        UE_LOGI("grab_test: HOST SCREENSHOT 3 (post-throw, mid-flight) saved=%d", ok);
    }

    // ---- 7. (HOST + heavy prop nearby) Heavy-grab arm.
    if (pr->heavyProp && pr->heavyMesh) {
        UE_LOGI("grab_test: now driving HEAVY arm (PCC.SetConstrainedComponents + BreakConstraint)");
        struct HeavyResolved {
            void* heavyGrab = nullptr;
            void* pccCls = nullptr;
            void* setConstrainedFn = nullptr;
            void* breakFn = nullptr;
            int32_t scFrame = 0;
            int32_t scPComp1 = -1, scPBone1 = -1, scPComp2 = -1, scPBone2 = -1;
            bool ok = false;
        };
        auto hr = std::make_shared<HeavyResolved>();
        done->store(0);
        GT::Post([rsv, hr, done] {
            hr->heavyGrab = ue_wrap::engine::ReadMainPlayerHeavyGrabPCC(rsv->player);
            if (!hr->heavyGrab) { UE_LOGW("grab_test: mainPlayer.heavyGrab is null"); done->store(2); return; }
            hr->pccCls = R::FindClass(P::name::PhysicsConstraintComponentClass);
            hr->setConstrainedFn = R::FindFunction(hr->pccCls, P::name::SetConstrainedComponentsFn);
            hr->breakFn          = R::FindFunction(hr->pccCls, P::name::BreakConstraintFn);
            if (!hr->setConstrainedFn || !hr->breakFn) {
                UE_LOGW("grab_test: PCC UFunction lookup failed (set=%p break=%p)",
                        hr->setConstrainedFn, hr->breakFn);
                done->store(2); return;
            }
            hr->scFrame  = R::FunctionFrameSize(hr->setConstrainedFn);
            hr->scPComp1 = R::FindParamOffset(hr->setConstrainedFn, L"Component1");
            hr->scPBone1 = R::FindParamOffset(hr->setConstrainedFn, L"BoneName1");
            hr->scPComp2 = R::FindParamOffset(hr->setConstrainedFn, L"Component2");
            hr->scPBone2 = R::FindParamOffset(hr->setConstrainedFn, L"BoneName2");
            UE_LOGI("grab_test: PCC resolve heavyGrab=%p pccCls=%p setFn=%p frame=%d "
                    "(C1@%d B1@%d C2@%d B2@%d) breakFn=%p",
                    hr->heavyGrab, hr->pccCls, hr->setConstrainedFn, hr->scFrame,
                    hr->scPComp1, hr->scPBone1, hr->scPComp2, hr->scPBone2, hr->breakFn);
            hr->ok = (hr->scPComp1 >= 0 && hr->scPBone1 >= 0 && hr->scPComp2 >= 0 && hr->scPBone2 >= 0);
            done->store(hr->ok ? 1 : 2);
        });
        while (done->load() == 0) ::Sleep(5);
        if (hr->ok) {
            done->store(0);
            GT::Post([rsv, pr, hr, done] {
                std::vector<uint8_t> frame(static_cast<size_t>(hr->scFrame), 0);
                *reinterpret_cast<void**>(frame.data() + hr->scPComp1) = nullptr;
                *reinterpret_cast<R::FName*>(frame.data() + hr->scPBone1) = R::FName{0, 0};
                *reinterpret_cast<void**>(frame.data() + hr->scPComp2) = pr->heavyMesh;
                *reinterpret_cast<R::FName*>(frame.data() + hr->scPBone2) = R::FName{0, 0};
                const bool ok = R::CallFunction(hr->heavyGrab, hr->setConstrainedFn, frame.data());
                UE_LOGI("grab_test: CallFunction(PCC.SetConstrainedComponents) -> %d "
                        "(expect grab_hook[PCC.SetConstrainedComponents])", ok);
                done->store(1);
            });
            while (done->load() == 0) ::Sleep(5);

            ::Sleep(1000);

            done->store(0);
            GT::Post([hr, done] {
                const bool ok = R::CallFunction(hr->heavyGrab, hr->breakFn, nullptr);
                UE_LOGI("grab_test: CallFunction(PCC.BreakConstraint) -> %d "
                        "(expect grab_hook[PCC.BreakConstraint PRE])", ok);
                done->store(1);
            });
            while (done->load() == 0) ::Sleep(5);
        }
    } else {
        UE_LOGI("grab_test: no nearby heavy prop -- skipping heavy-grab arm");
    }

    // ---- 8. (HOST) Timeline-force arm.
    UE_LOGI("grab_test: trying to force `grab` Timeline play (closes the last 3 BP-Timeline observer gaps)");
    done->store(0);
    GT::Post([rsv, done] {
        void* timeline = ue_wrap::engine::ReadMainPlayerGrabTimeline(rsv->player);
        if (!timeline) {
            UE_LOGW("grab_test: mainPlayer.grab Timeline is null -- skipping force-play");
            done->store(2); return;
        }
        void* tlCls = R::FindClass(P::name::TimelineComponentClass);
        void* playFromStartFn = R::FindFunction(tlCls, P::name::TimelinePlayFromStartFn);
        if (!playFromStartFn) {
            UE_LOGW("grab_test: TimelineComponent.PlayFromStart UFunction not found");
            done->store(2); return;
        }
        UE_LOGI("grab_test: forcing grab Timeline=%p .PlayFromStart()", timeline);
        const bool ok = R::CallFunction(timeline, playFromStartFn, nullptr);
        UE_LOGI("grab_test: CallFunction(grab.PlayFromStart) -> %d "
                "(expect grab_hook[grab.Update] x3 + grab_hook[grab.Finished PRE])", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);
    ::Sleep(3000);

    UE_LOGI("grab_test: DONE -- autonomous grab routine complete");
}

DWORD WINAPI GrabTestThread(LPVOID /*arg*/) {
    RunAutonomousGrabTest();
    return 0;
}

}  // namespace harness::autotest
