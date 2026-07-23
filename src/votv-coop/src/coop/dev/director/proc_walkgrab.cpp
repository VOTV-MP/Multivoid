// coop/dev/director/proc_walkgrab.cpp -- the walked-grab process set (Baritone
// IBaritoneProcess analogs). Three decision units the brain arbitrates by priority +
// context-activation:
//
//   ClearHandProcess (prio 100) -- active while the hand is FULL. Drops the held prop at
//     the input seam (InpActEvt_drop), effect-seam backstop if the input stub is inert.
//     PREEMPTS Goto/Grab: the bot cannot walk while holding a prop (measured 2026-07-23),
//     so it must empty its hand first. This is the state-awareness the held-disc lesson
//     demanded -- structural, not a bolted-on step.
//   GotoProcess    (prio 50)  -- active while out of reach. FindPath over the baked NavMesh
//     + per-tick AddMovementInput steering along the waypoints; waypoint-progress stuck
//     detection (distance-to-pile alone false-fails a winding route).
//   GrabProcess    (prio 40)  -- active while in reach + not yet grabbed. Settle, aim, then
//     the proven chippile grab (force lookAtActor + InpActEvt_use + playerGrabbed). Sets
//     goal.grabbed on success.
//
// Every action is at the human-INPUT seam (drive-at-input-seam). All OnTick runs on the
// game thread (inside the ControlManager's per-tick closure), so engine calls are direct.

#include "coop/dev/director/director.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
namespace D = ue_wrap::door;

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

constexpr float kAdvanceCm      = 70.f;   // hug the path: advance only when CLOSE, so the straight line to
                                          // the next waypoint stays on the walkable navmesh segment (a wide
                                          // radius cuts corners into walls -- the door-pin bug, 2026-07-23)
constexpr float kStuckImproveCm = 2.f;   // accept SLOW progress: shoving physics boxes out of the path
                                         // creeps forward a few cm at a time ("almost slid through" -- keep
                                         // pushing, don't quit). A real wall makes zero progress -> still stuck.
constexpr int   kStuckTicks     = 200;   // ~0.8 s of ZERO progress before an unstick attempt (patient)
constexpr float kDoorReachCm    = 320.f; // a closed door within this of the stuck bot is the blocker -> open it
constexpr int   kMaxDoorOpens   = 20;    // grind: keep opening doors as needed (never-give-up rule)
constexpr int   kUnstickTicks   = 70;    // ~0.28 s of sideways juke to slide off a box before re-checking
constexpr int   kSettleTicks    = 12;    // ~0.25 s braking before the grab
constexpr int   kVerifyTicks    = 90;    // ~1.8 s polling for the held clump
constexpr int   kDropMeasureTicks = 20;  // ~0.4 s to MEASURE if the input-seam drop cleared the hand

// ---- ClearHandProcess ------------------------------------------------------------------
// Drive-at-input-seam FIRST: press InpActEvt_drop (the key a human presses), then MEASURE
// whether the hand actually cleared. Do NOT mask with a mutator blindly (that was a RULE-1
// crutch on an unmeasured fact -- caught 2026-07-23). If the input seam is INERT (the
// reflection stub does not run the drop body -- the chippile InpActEvt_use-body-inert
// precedent), that is a HALT-fact we LABEL + record, then fall back to the proven effect-seam
// release so the run can proceed. `usedEffectFallback` is surfaced in the verdict.
bool g_clearHandUsedEffectFallback = false;   // read by the scenario verdict (drive-at-seam honesty)

class ClearHandProcess : public IProcess {
public:
    explicit ClearHandProcess(DirectorGoal& goal) : goal_(goal) {}
    const char* Name() const override { return "ClearHand"; }
    int Priority() const override { return 100; }
    bool IsActive(const PlayerContext& ctx) const override {
        // Clear a PRE-EXISTING held item ONLY while walking to the target (out of grab range). Once in
        // range the held item is (or becomes) the GOAL grab -- clearing it there undid a successful grab
        // (measured 2026-07-23: the grabbed clump landed in grabbing_actor, ClearHand woke and dropped it).
        return ctx.HandFull() && !goal_.grabbed && HorizDist(ctx.pos, goal_.targetPos) > goal_.reachCm;
    }
    ProcStatus OnTick(const PlayerContext& ctx) override {
        ++ticks_;
        if (ticks_ == 1) {   // input seam ONLY -- the key a human presses to drop (measure it first)
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* dropFn = cls ? R::FindFunction(cls, L"InpActEvt_drop_K2Node_InputActionEvent_1") : nullptr;
            if (dropFn) {
                const int32_t fs = R::FunctionFrameSize(dropFn);
                std::vector<uint8_t> f(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
                R::CallFunction(ctx.player, dropFn, f.empty() ? nullptr : f.data());
            }
            UE_LOGI("director/ClearHand: hand full (grabbing=%p holding=%p place=%d) -- input-seam "
                    "InpActEvt_drop fn=%p; MEASURING", ctx.held, ctx.holding, ctx.placeMode ? 1 : 0, dropFn);
            return ProcStatus::Working;
        }
        if (ticks_ == kDropMeasureTicks && !measured_) {   // MEASURE the input-seam result, don't assume
            measured_ = true;
            if (!ctx.HandFull()) {
                UE_LOGI("director/ClearHand: input-seam InpActEvt_drop WORKED -- hand cleared (measured); "
                        "no effect-seam needed");
                return ProcStatus::Working;
            }
            // MEASURED HALT-fact, LABELED (not a silent mask): the reflection input stub does not run the
            // drop body (chippile InpActEvt_use precedent) -> the drop is not input-seam-faithful. Clear at
            // the effect seam, ONE reasoned verb per slot: grabbing_actor -> PHC release; holding_actor
            // (the hand item / morph carry) -> throwHoldingProp (the SDK's "drop the held prop").
            g_clearHandUsedEffectFallback = true;
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* throwFn = cls ? R::FindFunction(cls, L"throwHoldingProp") : nullptr;
            UE_LOGW("director/ClearHand: InpActEvt_drop INERT (measured) -- effect-seam clear: "
                    "release(grabbing=%p) + throwHoldingProp(holding=%p) fn=%p", ctx.held, ctx.holding, throwFn);
            if (ctx.held) E::ReleaseMainPlayerGrabIfHolding(ctx.player, ctx.held);
            if (ctx.holding && throwFn) { ue_wrap::ParamFrame pf(throwFn); ue_wrap::Call(ctx.player, pf); }
            return ProcStatus::Working;
        }
        if (ticks_ == kDropMeasureTicks * 2 && ctx.HandFull()) {   // still full -> honest failure, not a silent deadline
            UE_LOGW("director/ClearHand: STILL full after effect-seam clear (grabbing=%p holding=%p place=%d) "
                    "-- no known verb emptied the slot; failing the run", ctx.held, ctx.holding, ctx.placeMode ? 1 : 0);
            return ProcStatus::Failed;
        }
        return ProcStatus::Working;   // IsActive() flips false once the hand clears -> we yield
    }
    void OnLostControl() override {
        if (ticks_ > 0)
            UE_LOGI("director/ClearHand: hand cleared after %d ticks (input-seam-faithful=%d) -- yielding",
                    ticks_, g_clearHandUsedEffectFallback ? 0 : 1);
    }
private:
    DirectorGoal& goal_;
    int  ticks_ = 0;
    bool measured_ = false;
};

// ---- GotoProcess -----------------------------------------------------------------------
class GotoProcess : public IProcess {
public:
    explicit GotoProcess(DirectorGoal& goal) : goal_(goal) {}
    const char* Name() const override { return "Goto"; }
    int Priority() const override { return 50; }
    bool IsActive(const PlayerContext& ctx) const override {
        return HorizDist(ctx.pos, goal_.targetPos) > goal_.reachCm;
    }
    ProcStatus OnTick(const PlayerContext& ctx) override {
        if (!pathed_) {   // compute the route once (after the hand is clear, so from the real start)
            pathed_ = true;
            std::vector<ue_wrap::FVector> path;
            const bool ok = E::FindNavPath(ctx.player, ctx.pos, goal_.targetPos, path);
            if (ok && path.size() >= 2) for (size_t i = 1; i < path.size(); ++i) waypoints_.push_back(path[i]);
            else waypoints_.push_back(goal_.targetPos);   // straight-line fallback
            UE_LOGI("director/Goto: route %zu waypoints (%s), dist=%.0fcm", waypoints_.size(),
                    ok ? "NavMesh route" : "straight-line fallback", HorizDist(ctx.pos, goal_.targetPos));
        }
        const float toPile = HorizDist(ctx.pos, goal_.targetPos);
        while (wp_ + 1 < waypoints_.size() && HorizDist(ctx.pos, waypoints_[wp_]) < kAdvanceCm) ++wp_;
        const float toWp = HorizDist(ctx.pos, waypoints_[wp_]);
        // Progress = advanced a waypoint OR closer to the current waypoint OR closer to the pile.
        bool progressed = false;
        if (wp_ > lastWp_) { progressed = true; lastWp_ = wp_; bestWp_ = 1e9f; }
        if (toWp   < bestWp_   - kStuckImproveCm) { bestWp_   = toWp;   progressed = true; }
        if (toPile < bestPile_ - kStuckImproveCm) { bestPile_ = toPile; progressed = true; }
        if (progressed) sinceProgress_ = 0;
        else if (++sinceProgress_ >= kStuckTicks) {
            sinceProgress_ = 0;
            // FIRST: a CLOSED door? Open it like a player pressing E (the door's own verb -- InpActEvt_use
            // is inert via reflection) and keep walking through.
            if (doorOpens_ < kMaxDoorOpens && TryOpenBlockingDoor(ctx)) { ++doorOpens_; return ProcStatus::Working; }
            // NEVER GIVE UP (USER RULE 2026-07-23): grind past physics boxes/clutter. Alternate a sideways
            // JUKE to slide off the obstacle, and RE-PATH periodically. Goto returns Failed ONLY on a hard
            // engine problem -- reaching the pile (IsActive->false, Grab takes over) or the run DEADLINE
            // (>= 30 s of grinding) are the terminators, not an early stuck-count.
            ++stuckEpisodes_;
            if (stuckEpisodes_ % 4 == 0) {   // periodic fresh route from where we actually are
                pathed_ = false; waypoints_.clear(); wp_ = 0; lastWp_ = 0; bestWp_ = 1e9f; bestPile_ = 1e9f;
                UE_LOGW("director/Goto: persistent stuck (toPile=%.0f) -- RE-PATH (episode %d, never give up)", toPile, stuckEpisodes_);
                return ProcStatus::Working;
            }
            jukeSign_ = -jukeSign_;            // try the other side each time
            unstickTicks_ = kUnstickTicks;
            UE_LOGW("director/Goto: stuck (toPile=%.0f) -- UNSTICK juke (episode %d, grinding, never give up)", toPile, stuckEpisodes_);
        }
        // Steer toward the current waypoint (horizontal) + FACE that direction (turns the body/camera, threads
        // doorways head-on). While UNSTICKING, add a sideways component to slide off a box instead of pinning.
        const ue_wrap::FVector wp = waypoints_[wp_];
        ue_wrap::FVector dir{ wp.X - ctx.pos.X, wp.Y - ctx.pos.Y, 0.f };
        float h = std::sqrt(dir.X * dir.X + dir.Y * dir.Y);
        if (h > 1.f) { dir.X /= h; dir.Y /= h; }
        if (unstickTicks_ > 0) {
            --unstickTicks_;
            const ue_wrap::FVector perp{ -dir.Y, dir.X, 0.f };   // horizontal perpendicular
            dir.X += perp.X * jukeSign_ * 0.9f; dir.Y += perp.Y * jukeSign_ * 0.9f;
            h = std::sqrt(dir.X * dir.X + dir.Y * dir.Y);
            if (h > 1.f) { dir.X /= h; dir.Y /= h; }
        }
        ue_wrap::FRotator face{};
        face.Yaw = std::atan2(dir.Y, dir.X) * (180.f / 3.14159265358979323846f);
        if (void* ctrl = E::GetController(ctx.player)) E::SetControlRotation(ctrl, face);
        E::AddMovementInput(ctx.player, dir, 1.0f, true);
        if ((++logTick_ % 60) == 0)
            UE_LOGI("director/Goto: toPile=%.0f toWp=%.0f wp=%zu/%zu ep=%d", toPile, toWp, wp_, waypoints_.size(), stuckEpisodes_);
        return ProcStatus::Working;
    }
private:
    // Find the nearest CLOSED, openable door within reach of the stuck bot and open it (the native swing,
    // bypass=true -- the same thing the door's own use does when a player presses E; InpActEvt_use's body
    // is inert via reflection). One-shot GUObjectArray scan, only on STUCK (not per frame -- perf rule).
    bool TryOpenBlockingDoor(const PlayerContext& ctx) {
        if (!D::EnsureResolved()) return false;
        void* best = nullptr; float bestDist = 1e9f;
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o) || !D::IsDoor(o)) continue;
            if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
            const float d = HorizDist(E::GetActorLocation(o), ctx.pos);
            if (d < bestDist) { bestDist = d; best = o; }
        }
        if (!best || bestDist > kDoorReachCm) return false;
        bool open = false;
        if (D::TryReadOpen(best, open) && open) return false;   // already open -> not the blocker
        if (!D::CanOpen(best)) {
            UE_LOGW("director/Goto: blocking door %p (%.0fcm) is LOCKED/jammed (CanOpen=false)", best, bestDist);
            return false;
        }
        D::CallDoorOpen(best, /*bypass=*/true);
        UE_LOGI("director/Goto: opened a closed door %p at %.0fcm (native swing, like pressing E) -- walking through",
                best, bestDist);
        return true;
    }

    DirectorGoal& goal_;
    bool   pathed_ = false;
    std::vector<ue_wrap::FVector> waypoints_;
    size_t wp_ = 0, lastWp_ = 0;
    float  bestWp_ = 1e9f, bestPile_ = 1e9f;
    int    sinceProgress_ = 0, logTick_ = 0, doorOpens_ = 0;
    int    stuckEpisodes_ = 0, unstickTicks_ = 0;
    float  jukeSign_ = 1.f;
};

// ---- GrabProcess -----------------------------------------------------------------------
class GrabProcess : public IProcess {
public:
    explicit GrabProcess(DirectorGoal& goal) : goal_(goal) {}
    const char* Name() const override { return "Grab"; }
    int Priority() const override { return 40; }
    bool IsActive(const PlayerContext& ctx) const override {
        return !goal_.grabbed && HorizDist(ctx.pos, goal_.targetPos) <= goal_.reachCm;
    }
    ProcStatus OnTick(const PlayerContext& ctx) override {
        ++ticks_;
        if (ticks_ < kSettleTicks) return ProcStatus::Working;   // brake (no input) before grabbing
        if (!grabIssued_) {
            grabIssued_ = true;
            // Aim at the pile.
            const ue_wrap::FVector cam = E::GetCameraLocation();
            E::SetControlRotation(E::GetController(ctx.player), LookAt(cam, goal_.targetPos));
            // Arm the real recognition path + run the proven conversion (chippile sequence).
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* useFn = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
            E::WriteMainPlayerLookAtActor(ctx.player, goal_.targetActor);
            bool armOk = false;
            if (useFn) {
                const int32_t fs = R::FunctionFrameSize(useFn);
                std::vector<uint8_t> f(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
                armOk = R::CallFunction(ctx.player, useFn, f.empty() ? nullptr : f.data());
            }
            void* pileCls = R::ClassOf(goal_.targetActor);
            void* grabFn = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
            bool callOk = false;
            if (grabFn) {
                ue_wrap::ParamFrame pf(grabFn);
                pf.Set<void*>(L"Player", ctx.player);
                callOk = ue_wrap::Call(goal_.targetActor, pf);   // the pile self-destructs here
            }
            UE_LOGI("director/Grab: settled, grabbing -- arm(InpActEvt_use)=%d playerGrabbed fn=%p call=%d",
                    armOk ? 1 : 0, grabFn, callOk ? 1 : 0);
            return ProcStatus::Working;
        }
        // Verify a clump landed in the grab state.
        E::MainPlayerGrabState gs{};
        if (E::ReadMainPlayerGrabState(ctx.player, gs)) {
            if ((gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ||
                (gs.holdingActor  && ue_wrap::prop::IsGarbageClump(gs.holdingActor))) {
                goal_.grabbed = true;
                UE_LOGI("director/Grab: VERIFIED -- clump in hand, goal GRABBED");
                return ProcStatus::Done;
            }
        }
        if (++verifyTicks_ >= kVerifyTicks) {
            UE_LOGW("director/Grab: no clump after %d verify ticks -- grab did not land", verifyTicks_);
            return ProcStatus::Failed;
        }
        return ProcStatus::Working;
    }
private:
    DirectorGoal& goal_;
    int  ticks_ = 0, verifyTicks_ = 0;
    bool grabIssued_ = false;
};

// ---- ReachProcess ----------------------------------------------------------------------
// The walk-TO terminal (no grab): once within reach, settle briefly (kill residual momentum so
// a later interaction is from a stationary body, mirroring the grab's SETTLE gate), then set
// goal.reached to terminate the run. The scenario does its OWN interaction after Run returns.
class ReachProcess : public IProcess {
public:
    explicit ReachProcess(DirectorGoal& goal) : goal_(goal) {}
    const char* Name() const override { return "Reach"; }
    int Priority() const override { return 40; }
    bool IsActive(const PlayerContext& ctx) const override {
        return !goal_.reached && HorizDist(ctx.pos, goal_.targetPos) <= goal_.reachCm;
    }
    ProcStatus OnTick(const PlayerContext& ctx) override {
        if (++ticks_ < kSettleTicks) return ProcStatus::Working;   // brake (no input) before yielding
        goal_.reached = true;
        UE_LOGI("director/Reach: within %.0fcm (dist=%.0f) + settled -- TARGET REACHED, yielding to the scenario",
                goal_.reachCm, HorizDist(ctx.pos, goal_.targetPos));
        return ProcStatus::Done;
    }
private:
    DirectorGoal& goal_;
    int ticks_ = 0;
};

}  // namespace

void AddWalkGrabProcesses(ControlManager& mgr, DirectorGoal& goal) {
    g_clearHandUsedEffectFallback = false;   // reset per run
    mgr.Add(std::make_unique<ClearHandProcess>(goal));
    mgr.Add(std::make_unique<GotoProcess>(goal));
    mgr.Add(std::make_unique<GrabProcess>(goal));
}

void AddWalkToProcesses(ControlManager& mgr, DirectorGoal& goal) {
    g_clearHandUsedEffectFallback = false;   // reset per run
    mgr.Add(std::make_unique<ClearHandProcess>(goal));
    mgr.Add(std::make_unique<GotoProcess>(goal));
    mgr.Add(std::make_unique<ReachProcess>(goal));
}

// Honesty accessor for the verdict: did ClearHand have to fall back to the effect seam because
// the input-seam drop was measured inert (i.e. the drop was NOT authority-equivalent to a human)?
bool DidClearHandUseEffectFallback() { return g_clearHandUsedEffectFallback; }

}  // namespace coop::director
