// harness/autotest_clump.cpp -- garbage-clump autonomous tests: the held-clump
// ATTACH e2e (VOTVCOOP_RUN_CLUMP_TEST) + the clump VISIBILITY probe
// (VOTVCOOP_RUN_CLUMPVIS_PROBE). Extracted verbatim from harness/autotest.cpp
// (2026-07-19 dissolve); interfaces + per-routine docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

}  // namespace

// --- Autonomous held-clump ATTACH e2e test (VOTVCOOP_RUN_CLUMP_TEST=1) --------
//
// Verifies the v26 MANNEQUIN-model clump sync on a real LAN pair. The clump is
// non-keyable (autotest 33e7f25), so it rides the existing prop pose pipeline
// identified by our EID, not a Key. The real trash-collect (trashBitsPile::
// playerTryToCollect) is BP-internal so a UFunction call can't trigger it; instead
// the HOST spawns a prop_garbageClump_C and writes it to the LOCAL player's
// grabbing_actor -- net_pump's held-edge then broadcasts a PropSpawn (key=None +
// eid) + streams PropPose keyed by that eid, exactly as a real grab would. On
// release (grabbing_actor cleared) it sends PropRelease. CLIENT is scan-only (the
// spawn + drive + release arrive via the wire). PASS = the CLIENT log shows
// "remote_prop::OnSpawn: spawned ... 'prop_garbageClump_C'" + "GRAB-IN key='None'
// eid=N -> ... clump kinematic" + "drive #N", and NEITHER peer crashes (the whole
// point after the 2a UAF). The HOST log shows "prop-mirror=BROADCAST" + "PropPose
// emit ... eid=N". (The synthetic cold-grab clump self-frees after ~1s, so the
// drive is brief; a real grabbed clump persists -- hands-on for the full visual.)
void RunAutonomousClumpTest() {
    const bool isHost = (cfg::ReadEnv("VOTVCOOP_NET_ROLE") != "client");
    if (!isHost) {
        UE_LOGI("clump_test: CLIENT scan-only -- verify in THIS log: 'remote_prop::OnSpawn "
                "spawned ... prop_garbageClump_C', 'GRAB-IN ... eid=N ... clump kinematic', "
                "'drive #N', and NO crash");
        return;
    }
    // Wait long enough for the CLIENT to fully settle, not just connect: a client
    // that just spawned its host puppet is still draining its ~2300-prop snapshot
    // (CPU contention) and BeginDeferredActorSpawnFromClass REFUSES mid-transition,
    // so the mirror SpawnActor (OnSpawn) returns null and the clump can't mirror.
    // 35 s gives the client time to drain so the run deterministically exercises the
    // spawn + eid-drive path.
    UE_LOGI("clump_test: HOST starting (waiting 35 s for world + client connect + settle)");
    ::Sleep(35000);

    struct Resolved { void* player = nullptr; void* clump = nullptr;
                      ue_wrap::FVector base{}; bool ok = false; };
    auto rsv  = std::make_shared<Resolved>();
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([rsv, done] {
        void* player = coop::players::Registry::Get().Local();  // the SAME player the send reads
        if (!player || !R::IsLive(player)) { UE_LOGW("clump_test: no live local player"); done->store(2); return; }
        void* cls = R::FindClass(L"prop_garbageClump_C");
        if (!cls) { UE_LOGW("clump_test: prop_garbageClump_C class not loaded -- aborting"); done->store(2); return; }
        const ue_wrap::FVector pLoc = ue_wrap::engine::GetActorLocation(player);
        const ue_wrap::FVector fwd  = ue_wrap::engine::GetActorForwardVector(player);
        rsv->base = ue_wrap::FVector{ pLoc.X + fwd.X * 120.f, pLoc.Y + fwd.Y * 120.f, pLoc.Z + 60.f };
        void* clump = ue_wrap::engine::SpawnActor(cls, rsv->base);
        if (!clump) { UE_LOGW("clump_test: SpawnActor(prop_garbageClump_C) returned null"); done->store(2); return; }
        rsv->player = player;
        rsv->clump  = clump;
        ue_wrap::engine::WriteMainPlayerGrabbingPair(player, clump, nullptr);
        rsv->ok = true;
        UE_LOGI("clump_test: HOST spawned clump=%p at (%.0f,%.0f,%.0f) + wrote grabbing_actor -- "
                "expect 'NEW held actor cls=prop_garbageClump_C ... eid=N -> prop-mirror=BROADCAST' next",
                clump, rsv->base.X, rsv->base.Y, rsv->base.Z);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);
    if (done->load() != 1 || !rsv->ok) { UE_LOGW("clump_test: setup failed -- aborting"); return; }

    // Hold for 20 s, RE-ASSERTING grabbing_actor each tick (a cold-written grab can be
    // cleared by the BP, which would fire a spurious release edge). The CLIENT's mirror
    // follows the streamed clump world transform (PropPose by eid), floating where the
    // host carries it -- like the mannequin. This autonomous run verifies spawn + eid
    // drive + clean release + NO CRASH; the full visual (clump floats in front, throws)
    // is a hands-on check with a real persistent clump.
    UE_LOGI("clump_test: holding (grabbing_actor re-asserted) for 20 s -- client drives the "
            "clump mirror by eid (PropPose)");
    for (int i = 0; i < 200; ++i) {
        auto step = std::make_shared<std::atomic<int>>(0);
        GT::Post([rsv, step] {
            if (rsv->clump && R::IsLive(rsv->clump) && rsv->player && R::IsLive(rsv->player)) {
                ue_wrap::engine::WriteMainPlayerGrabbingPair(rsv->player, rsv->clump, nullptr);
            }
            step->store(1);
        });
        while (step->load() == 0) ::Sleep(2);
        ::Sleep(100);
    }
    GT::Post([rsv] {
        if (rsv->player && R::IsLive(rsv->player))
            ue_wrap::engine::WriteMainPlayerGrabbingPair(rsv->player, nullptr, nullptr);
    });
    UE_LOGI("clump_test: DONE -- released. PASS if the CLIENT logged 'remote_prop::OnSpawn "
            "spawned prop_garbageClump_C' + 'GRAB-IN eid=N clump kinematic' + 'drive #N' and neither peer crashed.");
}

DWORD WINAPI ClumpTestThread(LPVOID /*arg*/) {
    RunAutonomousClumpTest();
    return 0;
}

// --- Clump VISIBILITY probe (VOTVCOOP_RUN_CLUMPVIS_PROBE=1) -------------------
//
// Gates the mannequin-model clump rework: does a BARE SpawnActor(prop_garbageClump_C)
// have a rendered StaticMesh asset, or is it empty (mesh loaded from save data on
// pickup, which a keyless spawn never runs)? Spawns one ~150 cm in front of the
// player at camera height + logs its StaticMesh component's asset (null vs named) +
// holds it for a host screenshot. If the asset is non-null the mirror is visible
// for free (the earlier empty-hands was hand-attach occlusion); if null the mirror
// needs the source mesh copied over the wire. Solo (role-agnostic).
void RunClumpVisProbe() {
    UE_LOGI("clumpvis: starting (waiting 40 s for gameplay + load past the 999 placeholders)");
    ::Sleep(40000);
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        void* player = coop::players::Registry::Get().Local();
        if (!player || !R::IsLive(player)) { UE_LOGW("clumpvis: no live local player"); done->store(2); return; }
        void* cls = R::FindClass(L"prop_garbageClump_C");
        if (!cls) { UE_LOGW("clumpvis: prop_garbageClump_C class not loaded"); done->store(2); return; }
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(player);
        const ue_wrap::FVector fwd = ue_wrap::engine::GetActorForwardVector(player);
        const ue_wrap::FVector at{ loc.X + fwd.X * 150.f, loc.Y + fwd.Y * 150.f, loc.Z + 40.f };
        void* clump = ue_wrap::engine::SpawnActor(cls, at);
        if (!clump || !R::IsLive(clump)) { UE_LOGW("clumpvis: SpawnActor failed"); done->store(2); return; }
        // Read the clump's StaticMesh component @0x0230 + its asset via the REFLECTED
        // field offset. (GetStaticMesh is NOT a resolvable UFunction in 4.27 -- it gave
        // false-null on every prior probe, poisoning the "bare clump empty" conclusion.)
        void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(clump) + 0x0230);
        void* smcCls = R::FindClass(L"StaticMeshComponent");
        const int32_t meshOff = smcCls ? R::FindPropertyOffset(smcCls, L"StaticMesh") : -1;
        UE_LOGI("clumpvis: StaticMeshComponent.StaticMesh field offset = %d", meshOff);
        void* asset = (comp && R::IsLive(comp) && meshOff >= 0)
            ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(comp) + meshOff) : nullptr;
        UE_LOGI("clumpvis: bare clump @%p comp=%p asset(via offset)=%p name='%ls' -- %s",
                clump, comp, asset, asset ? R::ToString(R::NameOf(asset)).c_str() : L"<null>",
                asset ? "BARE CLUMP HAS A MESH (visible -- earlier empty-hands was occlusion!)"
                      : "BARE CLUMP EMPTY (mirror needs the source mesh)");

        // Render validation: grab ANY world mesh (lockers/walls) via the offset + copy it
        // onto the clump. If the clump then RENDERS, the mesh-copy mechanism is viable.
        void* srcMesh = nullptr; std::wstring srcCls;
        if (meshOff >= 0) {
            const int32_t nObj = R::NumObjects();
            for (int32_t i = 0; i < nObj && !srcMesh; ++i) {
                void* o = R::ObjectAt(i);
                if (!o || o == comp || !R::IsLive(o)) continue;
                const std::wstring cn = R::ClassNameOf(o);
                if (cn.find(L"StaticMeshComponent") == std::wstring::npos) continue;
                if (R::ToString(R::NameOf(o)).rfind(L"Default__", 0) == 0) continue;
                void* a = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(o) + meshOff);
                if (a) { srcMesh = a; srcCls = cn; }
            }
        }
        UE_LOGI("clumpvis: sample world mesh=%p (from '%ls')", srcMesh, srcCls.c_str());
        if (srcMesh && comp && R::IsLive(comp) && meshOff >= 0) {
            void* setFn = R::FindFunction(smcCls, L"SetStaticMesh");
            if (setFn) {
                ue_wrap::ParamFrame sf(setFn); sf.Set<void*>(L"NewMesh", srcMesh);
                const bool ok = ue_wrap::Call(comp, sf);
                UE_LOGI("clumpvis: COPIED mesh '%ls' onto the bare clump via SetStaticMesh (ok=%d) "
                        "-- if it RENDERS, mesh-copy is VIABLE", R::ToString(R::NameOf(srcMesh)).c_str(), ok ? 1 : 0);
            } else {
                *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(comp) + meshOff) = srcMesh;
                UE_LOGI("clumpvis: COPIED mesh '%ls' via direct field write (SetStaticMesh unresolved)",
                        R::ToString(R::NameOf(srcMesh)).c_str());
            }
        }
        UE_LOGI("clumpvis: CLUMPVIS READY -- capture the host window (clump floats ~150cm ahead)");
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);
    ::Sleep(35000);  // hold the clump in view for the screenshot
    UE_LOGI("clumpvis: DONE");
}

DWORD WINAPI ClumpVisProbeThread(LPVOID /*arg*/) {
    RunClumpVisProbe();
    return 0;
}

}  // namespace harness::autotest
