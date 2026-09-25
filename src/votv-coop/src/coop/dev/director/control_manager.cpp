// coop/dev/director/control_manager.cpp -- the priority-arbitrated tick loop (Baritone
// PathingControlManager analog). Each tick, ON THE GAME THREAD: refresh the context, pick
// the highest-priority ACTIVE process, run its OnTick. The worker thread only paces the
// loop + checks termination. Because the whole tick runs inside one game-thread closure,
// the processes call engine functions directly (they are already on the game thread).

#include "coop/dev/director/director.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

namespace coop::director {
namespace {
namespace GT = ue_wrap::game_thread;

constexpr int kTickMs      = 4;     // near frame rate: AddMovementInput must re-land each frame or the
                                    // CharacterMovement (which consumes+clears ControlInputVector per
                                    // frame ~9ms) brakes between inputs -> near-zero net speed (measured
                                    // -- a 20 ms tick gave ~5 cm/s). The game-thread round trip paces
                                    // the real rate; this just removes the extra sleep between frames.
std::atomic<uint32_t> g_walkEpoch{0};  // EndWalks moves it; a run ends when it passes the run's own
}  // namespace

uint32_t WalkEpoch() { return g_walkEpoch.load(std::memory_order_acquire); }

void EndWalks() { g_walkEpoch.fetch_add(1, std::memory_order_acq_rel); }

void ControlManager::Add(std::unique_ptr<IProcess> proc) { procs_.push_back(std::move(proc)); }

bool ControlManager::Run(DirectorGoal& goal, int maxSeconds) {
    // The deadline is wall time, not a tick count. A tick is a game-thread round trip -- a Post,
    // then 5 ms polls until the closure has run -- plus the pace below, so it costs about ten
    // milliseconds and never the four the pace alone suggests. Counting `maxSeconds * 1000 /
    // kTickMs` ticks therefore ran a 60 s deadline for about 150 s, long enough for a caller's
    // own run to be killed while the walk it gave up on was still grinding.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(maxSeconds);
    const uint32_t epoch = goal.epoch.value_or(WalkEpoch());
    const char* lastDriver = "";
    // Each tick waits until its closure has run or faulted (GT::RunAndWait), so no closure outlives
    // this frame, and the references it holds are always live.
    for (int tick = 0; std::chrono::steady_clock::now() < deadline; ++tick) {
        // The session this walk belongs to has ended: the pawn it drives is no longer the drill's.
        if (WalkEpoch() != epoch) {
            UE_LOGW("director: the walks were ended with their session -- run CANCELLED (tick=%d)", tick);
            if (!goal.failed) { goal.failed = true; goal.failReason = "walks_ended"; }
            return false;
        }
        const int r = GT::RunAndWait([this, &goal, &lastDriver](std::atomic<int>& d) {
            PlayerContext ctx;
            if (!ctx.Refresh()) {
                // A location read that fails is a faulted dispatch, and it repeats: the run ends on it
                // instead of waiting out its deadline.
                if (ctx.posUnread) { goal.failed = true; goal.failReason = "player_location_unread"; d.store(1); return; }
                d.store(2); return;   // no possessed player this tick -- retry
            }
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
        if (r == GT::kTaskFaulted) {   // what the tick did is unknown: the run ends on it, named
            UE_LOGW("director: a tick's game-thread task faulted -- run FAILED (tick=%d)", tick);
            if (!goal.failed) { goal.failed = true; goal.failReason = "gt_task_faulted"; }
            return false;
        }
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
