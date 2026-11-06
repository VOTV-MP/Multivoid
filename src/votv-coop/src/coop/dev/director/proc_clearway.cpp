// coop/dev/director/proc_clearway.cpp -- the clearway process (clearway.h): a physics prop on the walker's route is
// dragged aside with the player's physics grab, as a player moves a box out of a doorway.
//
//   Approach: a Goto leg of its own to the prop, to the grab's reach.
//   Settle, then Grab: the director's aimed grab (aimed_grab.h), the use key's chain on a prop the player's own trace
//     takes. The game's grab decides: a prop it does not take (Failed) stays the detour's, counted, said once.
//   Turn: the camera turns to the freer side of the line to the waypoint the walk was heading for (a knee-height trace
//     each way, kSideProbeCm), the held prop swinging after it. Both sides tight: back along the route.
//   Drag: a straight Goto leg of its own that way, up to kDragCm, so the prop leaves the route with the walker.
//   Release: dropGrabObject, the game's own let-go of grabbing_actor (the container and camera drills let go with it
//     alone), verified on the player's grabbing actor.
//
// Priority 110, above ClearHand (100), which drops what the hand holds. Active only while a carry is in hand, so
// nothing runs per tick otherwise. All OnTick runs on the game thread.

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/clearway.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/leg_blocker.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr float kGrabReachCm   = 140.f;   // the walk to the prop ends this near it: the grab race's reach for a held aim
constexpr int   kApproachTicks = 1500;    // a leg of its own to a prop at hand that takes longer is no leg: the detour's
constexpr int   kSettleTicks   = 12;      // braking before the aim, as the grab process settles
constexpr int   kTurnTicks     = 90;      // the held prop swings after the camera before the walk on (~1 s)
constexpr float kDragCm        = 200.f;   // the drag's leg at most: far enough that the prop leaves the route with the walker
constexpr float kDragReachCm   = 30.f;
constexpr float kRecarryMovedCm = 50.f;   // a prop carried before is carried again once it has moved this far since
constexpr float kSideProbeCm   = 300.f;  // each side is traced this far from the walker
constexpr float kSideMinFreeCm = 80.f;    // a side freer than this takes the drag; both under it: the backward leg
constexpr float kSideMarginCm  = 40.f;    // the drag stops this short of what the trace met
constexpr float kKneeBelowCentreCm = 50.f;  // the walker's knee line, under its centre (LegBlocker's)
constexpr int   kDragTicks     = 400;     // a drag that has not arrived by then is released where it stands
constexpr int   kReleaseTicks  = 30;      // dropGrabObject has run; the grab must read empty within this
constexpr float kRadToDeg      = 180.f / 3.14159265358979323846f;

std::wstring NameOfProp(void* prop) { return R::ToString(R::NameOf(prop)); }

enum class Phase { Idle, Approach, Settle, Grab, Turn, Drag, Release };

class ClearwayProcess : public IProcess {
public:
    explicit ClearwayProcess(DirectorGoal& goal) : goal_(goal) {}
    const char* Name() const override { return "Clearway"; }
    int Priority() const override { return 110; }
    bool IsActive(const PlayerContext&) const override {
        return phase_ != Phase::Idle || (goal_.clearway.request.Raw() && goal_.clearway.request.Get());
    }
    ProcStatus OnTick(const PlayerContext& ctx) override {
        if (phase_ == Phase::Idle && !Begin(ctx)) return ProcStatus::Working;
        void* prop = prop_.Get();
        if (!prop && phase_ <= Phase::Grab) {
            Abort(ctx, "the prop is gone");
            return ProcStatus::Working;
        }
        ++ticks_;
        switch (phase_) {
        case Phase::Approach: {
            E::TryGetActorLocation(prop, approach_.targetPos);   // a prop knocked about moves; its last place stands
            if (!walk_->IsActive(ctx)) {
                walk_->OnLostControl();
                phase_ = Phase::Settle;
                ticks_ = 0;
                return ProcStatus::Working;
            }
            if (ticks_ > kApproachTicks) {
                Abort(ctx, "no way to the prop's reach");
                return ProcStatus::Working;
            }
            return walk_->OnTick(ctx);
        }
        case Phase::Settle:
            if (ticks_ >= kSettleTicks) {
                if (ctx.HandFull()) { Abort(ctx, "the hand is full"); return ProcStatus::Working; }
                grab_.emplace(ctx.player, prop);
                phase_ = Phase::Grab;
                ticks_ = 0;
            }
            return ProcStatus::Working;
        case Phase::Grab: {
            const GrabState st = grab_->Tick();
            if (st == GrabState::Failed) {
                Abort(ctx, grab_->Why());
            } else if (st == GrabState::Grabbed) {
                ChooseDrag(ctx, prop);
                phase_ = Phase::Turn;
                ticks_ = 0;
            }
            return ProcStatus::Working;
        }
        case Phase::Turn: {
            // Off the route: the prop hangs in front of the camera, so it leaves the leg with the walker.
            ue_wrap::FRotator toward{};
            toward.Yaw = dragYawDeg_;
            if (void* ctrl = E::GetController(ctx.player)) E::SetControlRotation(ctrl, toward);
            if (ticks_ >= kTurnTicks) {
                const float rad = dragYawDeg_ / kRadToDeg;
                drag_ = DirectorGoal{};
                drag_.straight = true;
                drag_.reachCm = kDragReachCm;
                drag_.targetPos = {ctx.pos.X + dragCm_ * std::cos(rad), ctx.pos.Y + dragCm_ * std::sin(rad), ctx.pos.Z};
                walk_ = MakeGotoProcess(drag_, /*onLevel*/ true);
                phase_ = Phase::Drag;
                ticks_ = 0;
            }
            return ProcStatus::Working;
        }
        case Phase::Drag:
            if (!walk_->IsActive(ctx) || ticks_ > kDragTicks) {
                if (ticks_ > kDragTicks)
                    UE_LOGW("director/clearway: %ls drag ended short of its leg, released where it stands", name_.c_str());
                walk_->OnLostControl();
                walk_.reset();
                phase_ = Phase::Release;
                ticks_ = 0;
                return ProcStatus::Working;
            }
            return walk_->OnTick(ctx);
        case Phase::Release:
            if (ticks_ == 1 && !CallOnPlayer(ctx.player, L"dropGrabObject")) {
                UE_LOGW("director/clearway: dropGrabObject did not run -- the grab cannot be released");
                End(ctx);
                return ProcStatus::Failed;
            }
            if (!Grabbing(ctx.player)) {
                ++goal_.clearway.carries;
                UE_LOGI("director/clearway: dragged %ls aside (%d/%d)", name_.c_str(), goal_.clearway.carries, kMaxCarries);
                End(ctx);
            } else if (ticks_ > kReleaseTicks) {   // the grab stayed: the run cannot walk on, as ClearHand's own failure
                UE_LOGW("director/clearway: the grab was not released after dropGrabObject");
                End(ctx);
                return ProcStatus::Failed;
            }
            return ProcStatus::Working;
        case Phase::Idle: break;
        }
        return ProcStatus::Working;
    }
    void OnLostControl() override {
        if (walk_) walk_->OnLostControl();
    }

private:
    // Takes up the prop Goto offered. False when it died before this tick.
    bool Begin(const PlayerContext& ctx) {
        void* prop = goal_.clearway.request.Get();
        goal_.clearway.request.Reset();
        if (!prop) return false;
        goal_.clearway.MarkTried(prop);
        prop_.Set(prop);
        name_ = NameOfProp(prop);
        approach_ = DirectorGoal{};
        approach_.targetActor = prop;
        approach_.reachCm = kGrabReachCm;
        E::TryGetActorLocation(prop, approach_.targetPos);
        walk_ = MakeGotoProcess(approach_, /*onLevel*/ true);
        grab_.reset();
        phase_ = Phase::Approach;
        ticks_ = 0;
        UE_LOGI("director/clearway: taking %ls aside, it stands on the route (%.0f cm away, %d of %d carried)",
                name_.c_str(), std::hypot(approach_.targetPos.X - ctx.pos.X, approach_.targetPos.Y - ctx.pos.Y),
                goal_.clearway.carries, kMaxCarries);
        return true;
    }

    // How far the walker can go along the XY unit vector (ux, uy) at knee height, up to kSideProbeCm, the held prop
    // not counted. 0 when the trace does not resolve: a side that cannot be read is not a free one.
    float FreeCm(const PlayerContext& ctx, float ux, float uy, void* held) const {
        const ue_wrap::FVector from{ctx.pos.X, ctx.pos.Y, ctx.pos.Z - kKneeBelowCentreCm};
        const ue_wrap::FVector to{from.X + ux * kSideProbeCm, from.Y + uy * kSideProbeCm, from.Z};
        const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic: the set LegBlocker traces
        void* const ignore[1] = {held};
        ue_wrap::trace::Hit hit;
        if (!ue_wrap::trace::LineHitForObjects(ctx.player, from, to, types, 2, ignore, 1, &hit)) return 0.f;
        if (!hit.blocked) return kSideProbeCm;
        return std::hypot(hit.point.X - from.X, hit.point.Y - from.Y);
    }

    // Where the drag goes: sideways across the line to the waypoint the walk was heading for, to the side with the
    // longer free run, for at most kDragCm and short of what the trace met. Both sides tight: back along the route.
    void ChooseDrag(const PlayerContext& ctx, void* held) {
        const ue_wrap::FVector& to = goal_.clearway.heading;
        const float dx = to.X - ctx.pos.X, dy = to.Y - ctx.pos.Y;
        const float len = std::hypot(dx, dy);
        float left = 0.f, right = 0.f;
        if (len > 1.f) {
            // UE yaw turns clockwise seen from above: the right of (dx, dy) is (-dy, dx).
            right = FreeCm(ctx, -dy / len, dx / len, held);
            left  = FreeCm(ctx, dy / len, -dx / len, held);
        }
        const bool toRight = right >= left;
        const float free = toRight ? right : left;
        if (free >= kSideMinFreeCm) {
            const float sx = toRight ? -dy / len : dy / len, sy = toRight ? dx / len : -dx / len;
            dragYawDeg_ = std::atan2(sy, sx) * kRadToDeg;
            dragCm_ = (std::min)(kDragCm, free - kSideMarginCm);
            UE_LOGI("director/clearway: dragging %ls to the %s (free %.0f cm, other side %.0f cm)", name_.c_str(),
                    toRight ? "right" : "left", free, toRight ? left : right);
            return;
        }
        dragYawDeg_ = AwayFromHeading(ctx).Yaw;
        dragCm_ = kDragCm;
        UE_LOGW("director/clearway: both sides of %ls are tight (left %.0f cm, right %.0f cm free) -- dragging it "
                "back along the route", name_.c_str(), left, right);
    }

    // The camera yaw that looks from the waypoint the walk was heading for back through the walker.
    ue_wrap::FRotator AwayFromHeading(const PlayerContext& ctx) const {
        const ue_wrap::FVector& to = goal_.clearway.heading;
        ue_wrap::FRotator away{};
        away.Yaw = std::atan2(ctx.pos.Y - to.Y, ctx.pos.X - to.X) * kRadToDeg;
        return away;
    }

    // The attempt ends without the prop aside: the detour's, at the next persistent stuck. Said per attempt (a prop
    // is taken up again only once it has moved) and counted.
    void Abort(const PlayerContext& ctx, const char* why) {
        ++goal_.clearway.detoured;
        UE_LOGW("director/clearway: %ls stays where it is (%s) -- left to the detour (%d so far)", name_.c_str(), why,
                goal_.clearway.detoured);
        End(ctx);
    }

    void End(const PlayerContext&) {
        if (walk_) walk_->OnLostControl();
        walk_.reset();
        grab_.reset();
        prop_.Reset();
        phase_ = Phase::Idle;
        ++goal_.clearway.attempts;
    }

    DirectorGoal& goal_;
    Phase phase_ = Phase::Idle;
    int ticks_ = 0;
    float dragYawDeg_ = 0.f;   // the drag's heading and length, chosen when the grab holds
    float dragCm_ = kDragCm;
    ue_wrap::CachedObjRef prop_;
    std::wstring name_;
    DirectorGoal approach_;
    DirectorGoal drag_;
    std::unique_ptr<IProcess> walk_;
    std::optional<AimedGrab> grab_;
};

// The dev row's reading, once: the red arm of the D0 walk.
bool ClearwayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::director_clearway_off);
    return off;
}

}  // namespace

ClearwayBoard::TriedProp* ClearwayBoard::FindTried(void* prop) {
    tried_.erase(std::remove_if(tried_.begin(), tried_.end(), [](const TriedProp& t) { return !t.ref.Get(); }),
                 tried_.end());
    for (TriedProp& t : tried_)
        if (t.ref.Get() == prop) return &t;
    return nullptr;
}

void ClearwayBoard::MarkTried(void* prop, bool refused) {
    TriedProp* t = FindTried(prop);
    if (!t) {
        tried_.emplace_back();
        t = &tried_.back();
        t->ref.Set(prop);
    }
    t->refused = t->refused || refused;
    E::TryGetActorLocation(prop, t->at);
}

bool ClearwayBoard::Offer(void* prop, const ue_wrap::FVector& headingTo) {
    if (!armed || !prop || request.Get()) return false;
    if (carries >= kMaxCarries) {
        UE_LOGW("director/clearway: %d props carried already -- %ls is left to the detour", carries,
                NameOfProp(prop).c_str());
        return false;
    }
    if (const TriedProp* t = FindTried(prop)) {
        ue_wrap::FVector now{};
        const bool read = E::TryGetActorLocation(prop, now);
        const float movedCm = read ? std::hypot(std::hypot(now.X - t->at.X, now.Y - t->at.Y), now.Z - t->at.Z) : 0.f;
        if (t->refused || movedCm < kRecarryMovedCm) {
            UE_LOGW("director/clearway: %ls was taken up before and has not moved since (%.0f cm) -- left to the detour",
                    NameOfProp(prop).c_str(), movedCm);
            return false;
        }
        UE_LOGI("director/clearway: %ls blocks the route again, %.0f cm from where its last carry began -- carried again",
                NameOfProp(prop).c_str(), movedCm);
    }
    // The player's physics grab takes what a hand cannot hold (furniture, heavy props), so the in-hand hold flags are
    // not asked: AimedGrab's verify decides. Refused here is what the game's grab cannot move either, a child actor
    // (it rides its parent); a frozen or static prop never reaches Offer (IsPhysicsProp, leg_blocker.cpp:22).
    bool childRead = false;
    const bool child = E::IsChildActor(prop, childRead);
    if (!childRead || child) {
        MarkTried(prop, /*refused=*/true);
        ++detoured;
        UE_LOGW("director/clearway: %ls cannot be dragged (%s) -- left to the detour (%d so far)", NameOfProp(prop).c_str(),
                childRead ? "a child actor" : "its child flag is unread", detoured);
        return false;
    }
    request.Set(prop);
    heading = headingTo;
    return true;
}

std::unique_ptr<IProcess> MakeClearwayProcess(DirectorGoal& goal) {
    goal.clearway.armed = !ClearwayOff();
    if (!goal.clearway.armed) UE_LOGI("director/clearway: off (director_clearway_off) -- a blocking prop is the detour's");
    return std::make_unique<ClearwayProcess>(goal);
}

}  // namespace coop::director
