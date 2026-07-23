// coop/dev/director/control_manager.cpp -- the priority-arbitrated tick loop (Baritone
// PathingControlManager analog). Each tick, ON THE GAME THREAD: refresh the context, pick
// the highest-priority ACTIVE process, run its OnTick. The worker thread only paces the
// loop + checks termination. Because the whole tick runs inside one game-thread closure,
// the processes call engine functions directly (they are already on the game thread).

#include "coop/dev/director/director.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>
#include <utility>

namespace coop::director {
namespace {
namespace GT = ue_wrap::game_thread;

constexpr int kTickMs      = 4;     // near frame rate: AddMovementInput must re-land each frame or the
                                    // CharacterMovement (which consumes+clears ControlInputVector per
                                    // frame ~9ms) brakes between inputs -> near-zero net speed (measured
                                    // 2026-07-23: a 20ms tick gave ~5 cm/s). The RunGT round-trip paces
                                    // the real rate; this just removes the extra sleep between frames.
constexpr int kGtTimeoutMs = 4000;  // bound the GT wait -- a stalled game thread must NOT hang the run

// Returns the body's stored code (1 ok / 2 no-player), or 0 if the game thread did not run the
// posted closure within kGtTimeoutMs (a GT stall / dropped Post -- the `done` shared_ptr keeps the
// slot alive if the closure runs late, so no use-after-free).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    int waited = 0;
    while (done->load() == 0) {
        ::Sleep(5);
        waited += 5;
        if (waited >= kGtTimeoutMs) return 0;   // timeout
    }
    return done->load();
}
}  // namespace

void ControlManager::Add(std::unique_ptr<IProcess> proc) { procs_.push_back(std::move(proc)); }

bool ControlManager::Run(DirectorGoal& goal, int maxSeconds) {
    const int maxTicks = maxSeconds * 1000 / kTickMs;
    const char* lastDriver = "";
    int stall = 0;
    for (int tick = 0; tick < maxTicks; ++tick) {
        const int r = RunGT([this, &goal, &lastDriver](std::atomic<int>& d) {
            PlayerContext ctx;
            if (!ctx.Refresh()) { d.store(2); return; }   // no possessed player this tick -- retry
            // Highest-priority ACTIVE process wins control this tick (the arbiter).
            IProcess* active = nullptr;
            for (auto& up : procs_)
                if (up->IsActive(ctx) && (!active || up->Priority() > active->Priority())) active = up.get();
            if (active != inControl_) {
                if (inControl_) inControl_->OnLostControl();
                inControl_ = active;
                if (active && active->Name() != lastDriver) {
                    UE_LOGI("director: control -> %s (prio %d)", active->Name(), active->Priority());
                    lastDriver = active->Name();
                }
            }
            if (active) {
                const ProcStatus s = active->OnTick(ctx);
                if (s == ProcStatus::Failed && !goal.failed) { goal.failed = true; goal.failReason = active->Name(); }
            }
            d.store(1);
        });
        if (r == 0) {   // GT stall / dropped Post -- do not hang; abort after a few
            if (++stall >= 3) { UE_LOGW("director: game thread stalled 3x -- aborting run"); goal.failed = true; goal.failReason = "gt_stall"; return false; }
            UE_LOGW("director: GT tick timeout (%d/3)", stall);
            continue;
        }
        stall = 0;
        if (goal.grabbed) { UE_LOGI("director: GOAL REACHED -- run DONE (tick=%d)", tick); return true; }
        if (goal.reached) { UE_LOGI("director: TARGET REACHED (walk-to) -- run DONE (tick=%d)", tick); return true; }
        if (goal.failed)  { UE_LOGW("director: run FAILED reason=%s (tick=%d)", goal.failReason, tick); return false; }
        ::Sleep(kTickMs);
    }
    UE_LOGW("director: run DEADLINE (%d s) -- goal not reached", maxSeconds);
    if (!goal.failed) { goal.failed = true; goal.failReason = "deadline"; }
    return false;
}

}  // namespace coop::director
