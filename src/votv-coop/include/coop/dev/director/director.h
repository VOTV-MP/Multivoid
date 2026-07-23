// coop/dev/director/director.h -- the autonomous bot-director BRAIN (Baritone-analog).
//
// Design: research/findings/tooling/votv-baritone-analog-autonomous-director-DESIGN-
// 2026-07-23.md (Part A: the brain is the [ARCH] part of Baritone that ports; the voxel
// pathfinder is replaced by the engine's baked NavMesh). This is the minimal-but-real
// brain: a read-model (PlayerContext ~ IPlayerContext), priority-arbitrated processes
// (IProcess ~ IBaritoneProcess) driven by a tick loop (ControlManager ~
// PathingControlManager). Concrete behaviours plug in as processes; arbitration is
// EMERGENT from context (a full hand activates ClearHand, which preempts Goto) -- not a
// hardcoded step sequence. That is what makes the bot state-aware: it reads its OWN state
// and reacts, instead of blindly pushing input (the held-disc lesson, 2026-07-23).
//
// DEV-ONLY (RULE 3): the director exercises the game for autonomous testing; it is NOT in
// the shipping gameplay path. It drives the game ONLY at the human-INPUT seam
// (AddMovementInput, InpActEvt_*, forced aim) so the bot is authority-equivalent to a
// human client (drive-at-input-seam). All engine access is on the game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <windows.h>   // DWORD/WINAPI/LPVOID for the worker-thread entry

#include <cstdint>
#include <memory>
#include <vector>

namespace coop::director {

// The read-model (Baritone IPlayerContext analog): the bot's view of its OWN state,
// refreshed each tick on the game thread. A process reads this, never the engine directly.
struct PlayerContext {
    void*             player    = nullptr;   // possessed local mainPlayer_C (GetController()!=null)
    bool              possessed = false;
    ue_wrap::FVector  pos{};
    void*             held      = nullptr;   // grabbing_actor (PHC-held prop) => a full hand
    void*             holding   = nullptr;   // holding_actor (chipPile/clump morph carry)
    bool              placeMode = false;     // drop_place (R-hold placement mode)

    bool HandFull() const { return held != nullptr || holding != nullptr || placeMode; }

    // Populate from the live engine state. Game thread only. False if no possessed player.
    bool Refresh();
};

// The shared goal the process set reads + writes (the Blackboard). Set by the scenario.
struct DirectorGoal {
    void*             targetActor = nullptr;  // e.g. the chosen chipPile
    ue_wrap::FVector  targetPos{};
    float             reachCm     = 170.f;    // the interaction verb's OWN reach, not a nav constant
    bool              grabbed     = false;    // set by GrabProcess on success -> terminates the run
    bool              reached     = false;    // set by ReachProcess (walk-to, no grab) -> terminates the run
    bool              failed      = false;    // set by any process on a hard failure
    const char*       failReason  = "";
};

enum class ProcStatus { Idle, Working, Done, Failed };

// A decision unit competing for control (Baritone IBaritoneProcess analog). Higher
// Priority wins; the highest-priority process whose IsActive(ctx) is true drives the tick.
// The object PERSISTS across ticks, so per-behaviour state (a computed path, a settle
// timer, a retry latch) lives in its members.
class IProcess {
public:
    virtual ~IProcess() = default;
    virtual const char* Name() const = 0;
    virtual int         Priority() const = 0;
    virtual bool        IsActive(const PlayerContext& ctx) const = 0;
    virtual ProcStatus  OnTick(const PlayerContext& ctx) = 0;   // does its work (drives input at the seam)
    virtual void        OnLostControl() {}
};

// The priority-arbitrated tick loop (Baritone PathingControlManager analog). Each tick,
// on the game thread: refresh the context, pick the highest-priority ACTIVE process, run
// its OnTick. The worker thread only paces the loop + checks termination.
class ControlManager {
public:
    void Add(std::unique_ptr<IProcess> proc);
    // Drive until goal.grabbed / goal.failed / the deadline. Returns true iff goal.grabbed.
    bool Run(DirectorGoal& goal, int maxSeconds);
private:
    std::vector<std::unique_ptr<IProcess>> procs_;
    IProcess* inControl_ = nullptr;
};

// Build the walked-grab process set for `goal` and add it to `mgr`
// (ClearHand prio 100 > Goto prio 50 > Grab prio 40). In proc_walkgrab.cpp.
void AddWalkGrabProcesses(ControlManager& mgr, DirectorGoal& goal);

// Build the walk-TO process set (no grab): ClearHand prio 100 > Goto prio 50 > Reach prio 40.
// Reach sets goal.reached (terminating the run) after a brief settle once within goal.reachCm.
// Used by scenarios that walk to a target and then do their OWN interaction (the container
// probe). In proc_walkgrab.cpp.
void AddWalkToProcesses(ControlManager& mgr, DirectorGoal& goal);

// True iff the last run's ClearHand had to use the effect-seam release because the input-seam
// drop was MEASURED inert -- i.e. the drop was NOT input-seam-faithful (surfaced in the verdict).
bool DidClearHandUseEffectFallback();

// The scenario entry (director_run.cpp): resolve the player, pick an OPEN chipPile via the
// NavMesh, run the brain to grab it. Env-gated (VOTVCOOP_RUN_DIRECTOR_WALKGRAB=1).
void RunWalkGrabDirector();

// Worker-thread wrapper for the harness dispatch. Pass to ::CreateThread / SpawnIf.
DWORD WINAPI WalkGrabDirectorThread(LPVOID arg);

// The container-take INPUT PROBE (container_take_probe.cpp): resolve a placed, non-empty,
// nav-reachable world container, walk to it (the brain), then drive the FAITHFUL human take
// chain (openContainer -> slot pressButton -> em_take) and MEASURE whether the take executed
// (the container's GObjStack item count decremented). extract(Index) is a NON-FAITHFUL
// diagnostic fallback. Records the container's baked FName + a positive/negative control.
// A HALT-style drivability probe: its verdict decides whether the container concurrent-take
// race is buildable on the reflected-verb model. Env-gated (VOTVCOOP_RUN_CTAKE_PROBE=1), solo.
void RunContainerTakeProbe();
DWORD WINAPI ContainerTakeProbeThread(LPVOID arg);

}  // namespace coop::director
