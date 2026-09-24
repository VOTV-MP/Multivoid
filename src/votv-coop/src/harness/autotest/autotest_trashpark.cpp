// harness/autotest/autotest_trashpark.cpp -- the trash morph gate, shown red and green in one
// run (VOTVCOOP_RUN_TRASH_PARK=1 on both peers). The interface and the per-routine docs live in
// harness/autotest.h.
//
// Each peer calls the pile's own morph verb, `toClump`, on the nearest chip pile -- the verb the
// gate guards, and the one every in-game trigger ends in: the collision component's overlap
// handler and the arir follower both call it. The HOST's pile morphs and the actor is gone, which
// proves the verb does what the gate is refusing; the CLIENT's survives with its element id. A
// gate that cannot be shown firing passes forever, so the red arm is the point of the drill.
//
// Driving the verb rather than reproducing one caller is deliberate: a prop dropped onto a pile
// reached the overlap handler zero times in two measured runs, so a drill built on it would have
// reported the gate untested while looking like a pass.
#include "harness/autotest.h"

#include "coop/element/element.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <memory>
#include <vector>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// The finders take a radius; this drill wants the nearest one in the level.
constexpr float kAnywhereCm = 1.0e6f;

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail); engine state is
// game-thread only.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// The eid bound to a host pile (the forward map) or to a mirror (the wire); the invalid id when
// the pile is untracked.
coop::element::ElementId EidOf(void* pile) {
    coop::element::ElementId e = PT::GetPropElementIdForActor(pile);
    if (e == coop::element::kInvalidId) e = coop::remote_prop::ResolveMirrorEidByActor(pile);
    return e;
}

struct Subject {
    void* pile = nullptr;
    int32_t pileIdx = 0;
    coop::element::ElementId eid = coop::element::kInvalidId;
    ue_wrap::FVector pilePos{};
    void* toClumpFn = nullptr;
    int32_t frameSize = 0;
};

}  // namespace

void RunTrashParkProbe() {
    const bool isClient = IsClientRole();
    const char* who = isClient ? "CLIENT (the gate refuses)" : "HOST (the gate stands aside)";
    UE_LOGI("trash_park: %s -- waiting 70s for the join and the pile binds, then calling toClump "
            "on the nearest pile", who);
    ::Sleep(70000);

    auto sb = std::make_shared<Subject>();
    if (RunGT([sb](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (!p || !R::IsLive(p)) { UE_LOGW("trash_park: no local player"); d.store(2); return; }
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(p, at)) {
                UE_LOGW("trash_park: the player's location could not be read"); d.store(2); return; }
            sb->pile = ue_wrap::prop::FindNearestChipPile(at, kAnywhereCm, nullptr);
            if (!sb->pile) { UE_LOGW("trash_park: no chip pile in the level"); d.store(2); return; }
            sb->pileIdx = R::InternalIndexOf(sb->pile);
            sb->eid = EidOf(sb->pile);
            if (!E::TryGetActorLocation(sb->pile, sb->pilePos)) {
                UE_LOGW("trash_park: the pile's location could not be read"); d.store(2); return; }
            // The verb is declared on the pile's own class; resolving it through the dispatch
            // lookup names the declarer, so a variant that inherits it is still covered.
            void* declarer = nullptr;
            sb->toClumpFn = R::FindDispatchFunction(R::ClassOf(sb->pile), L"toClump", &declarer);
            if (!sb->toClumpFn) { UE_LOGW("trash_park: toClump did not resolve on the pile"); d.store(2); return; }
            sb->frameSize = R::FunctionFrameSize(sb->toClumpFn);
            UE_LOGI("trash_park: subject pile=%p eid=%u at(%.0f,%.0f,%.0f) toClump=%p frame=%d",
                    sb->pile, static_cast<unsigned>(sb->eid), sb->pilePos.X, sb->pilePos.Y, sb->pilePos.Z,
                    sb->toClumpFn, sb->frameSize);
            d.store(1);
        }) != 1) { UE_LOGW("trash_park: could not pick a subject -- aborting"); return; }

    // The call. The gate sits at the body's entry on every route, so a reflected call reaches it
    // exactly as the game's own does.
    RunGT([sb](std::atomic<int>& d) {
        std::vector<uint8_t> frame(sb->frameSize > 0 ? static_cast<size_t>(sb->frameSize) : 0, 0u);
        const bool ok = R::CallFunction(sb->pile, sb->toClumpFn, frame.empty() ? nullptr : frame.data());
        UE_LOGI("trash_park: >>> CALL toClump on pile %p (dispatched=%d) <<<", sb->pile, ok ? 1 : 0);
        d.store(1);
    });
    ::Sleep(2000);   // the body spawns the clump and destroys the pile in one call

    RunGT([sb](std::atomic<int>& d) {
        // IsLiveByIndex, never IsLive: a destroyed actor's slot can be recycled, and a raw
        // liveness read of freed memory misreads.
        const bool alive = R::IsLiveByIndex(sb->pile, sb->pileIdx);
        const bool stillPile = alive && ue_wrap::prop::IsChipPile(sb->pile);
        const coop::element::ElementId nowEid = alive ? EidOf(sb->pile) : coop::element::kInvalidId;
        const bool keptEid = (nowEid == sb->eid);
        const bool refused = IsClientRole();
        const bool asDesigned = refused ? (alive && stillPile && keptEid) : !alive;
        UE_LOGI("trash_park: VERDICT role=%s alive=%d stillPile=%d eid=%u->%u kept=%d -- %s",
                refused ? "client" : "host", alive ? 1 : 0, stillPile ? 1 : 0,
                static_cast<unsigned>(sb->eid), static_cast<unsigned>(nowEid), keptEid ? 1 : 0,
                asDesigned ? (refused ? "GREEN: the verb was refused and the pile is untouched"
                                      : "RED: the verb ran and the pile is gone, so it does morph")
                           : (refused ? "FAIL: a client authored its own morph"
                                      : "FAIL: the host's own pile did not morph -- read the gate's "
                                        "to-clump tally: at zero the call never reached the body"));
        d.store(1);
    });
    UE_LOGI("trash_park: done");
}

DWORD WINAPI TrashParkProbeThread(LPVOID) {
    RunTrashParkProbe();
    return 0;
}

}  // namespace harness::autotest
