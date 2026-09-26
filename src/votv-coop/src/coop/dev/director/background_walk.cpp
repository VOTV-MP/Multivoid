// coop/dev/director/background_walk.cpp -- a walk-to run on a worker thread; see director.h.

#include "coop/dev/director/director.h"

namespace coop::director {
namespace {

struct Start {
    std::shared_ptr<BackgroundWalk> walk;
    int deadlineS;
};

DWORD WINAPI WalkThread(LPVOID arg) {
    std::unique_ptr<Start> start(static_cast<Start*>(arg));
    BackgroundWalk& w = *start->walk;
    ControlManager mgr;
    if (w.carry) AddCarryToProcesses(mgr, w.goal);
    else AddWalkToProcesses(mgr, w.goal);
    mgr.Run(w.goal, start->deadlineS);
    w.state.store(w.goal.reached ? 1 : 2);
    return 0;
}

}  // namespace

std::shared_ptr<BackgroundWalk> StartBackgroundWalk(const ue_wrap::FVector& to, float reachCm, int deadlineS,
                                                    bool carry) {
    auto walk = std::make_shared<BackgroundWalk>();
    walk->goal.targetPos = to;
    walk->goal.reachCm = reachCm;
    walk->carry = carry;
    walk->goal.epoch = WalkEpoch();
    auto* start = new Start{walk, deadlineS};
    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, start, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete start;
        walk->state.store(2);
    }
    return walk;
}

}  // namespace coop::director
