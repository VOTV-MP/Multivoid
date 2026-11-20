// coop/dev/constraint_join_drill_carry.cpp -- the constraint-join drill's carry of box A by the bot director, run by whichever
// peer carries (the host in c1 and c3, the client in c2); see coop/dev/constraint_join_drill.h.

#include "constraint_join_drill_detail.h"

#include "coop/dev/drill_ground.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdio>

namespace coop::dev::constraint_join_drill::detail {
namespace {

namespace DG = coop::dev::drill_ground;
namespace DR = coop::director;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;

// A held prop hangs at the hand's point, which the camera's ray sets: the director aimed that ray down at the box on the
// floor, so the view is raised to this pitch, as a player lifts what it holds, until the held box stands kHeldOffCm from
// where it rested (the coast drill's lift, measured).
constexpr float kLiftViewPitchDeg = 15.f;

}  // namespace

void CarryAct::To(Step s) {
    step_ = s;
    stepMs_ = coop::net::NowMs();
}

bool CarryAct::Expired() const { return coop::net::NowMs() - stepMs_ > kBoundMs; }

void CarryAct::Begin(void* boxA, const ue_wrap::FVector& away, char who) {
    box_.Set(boxA);
    away_ = away;
    who_ = who;
    liftTicks_ = 0;
    To(Step::Start);
}

void CarryAct::Reset() {
    if (walk_) DR::CancelBackgroundWalk(*walk_);
    walk_.reset();
    grab_.reset();
    box_.Reset();
    step_ = Step::Idle;
    liftTicks_ = 0;
}

CarryAct::Status CarryAct::Tick() {
    if (step_ == Step::Idle || step_ == Step::Done) return Status::Done;
    void* player = coop::players::Registry::Get().Local();
    void* box = box_.Get();
    if (!player || !R::IsLive(player)) { Wait(who_, "this peer's pawn"); return Status::Working; }
    if (!box) {
        Invalid(who_, "box A died before the carry ended");
        return Status::Failed;
    }
    switch (step_) {
    case Step::Start: {
        ue_wrap::FVector at{}, origin{}, ext{};
        if (!E::TryGetActorLocation(box, at) || !E::GetActorBounds(box, true, origin, ext)) {
            Invalid(who_, "box A's place could not be read");
            return Status::Failed;
        }
        const float reach = ext.X + kStandGapCm;
        const ue_wrap::FVector stand{at.X + away_.X * reach, at.Y + away_.Y * reach, at.Z};
        walk_ = DR::StartBackgroundWalk(stand, kStandReachCm, kWalkDeadlineS, /*carry=*/false, box);
        UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] walks to (%.0f,%.0f,%.0f), beside box A at (%.0f,%.0f,%.0f)", who_,
                static_cast<double>(stand.X), static_cast<double>(stand.Y), static_cast<double>(stand.Z),
                static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z));
        To(Step::Walk);
        return Status::Working;
    }
    case Step::Walk: {
        const int st = walk_ ? walk_->state.load() : 2;
        if (st == 0) {
            Wait(who_, "the director's walk to box A");
            if (Expired()) { Invalid(who_, "the director's walk to box A never ended"); return Status::Failed; }
            return Status::Working;
        }
        walk_.reset();
        if (st != 1) { Invalid(who_, "the director could not reach box A"); return Status::Failed; }
        grab_ = std::make_unique<DR::AimedGrab>(player, box);
        To(Step::Grab);
        return Status::Working;
    }
    case Step::Grab:
        switch (grab_->Tick()) {
        case DR::GrabState::Working:
            if (Expired()) { Invalid(who_, "the grab of box A never took"); return Status::Failed; }
            return Status::Working;
        case DR::GrabState::Failed:
            if (grab_->NoAim()) Invalid(who_, "no aim in the fan took box A: %s", grab_->Why());
            else Invalid(who_, "the grab chain ran and left box A out of the hand: %s", grab_->Why());
            return Status::Failed;
        case DR::GrabState::Grabbed:
            UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] box A is in the hand after %d aim pose(s)", who_, grab_->AimPoses());
            grab_.reset();
            E::TryGetActorLocation(box, rest_);
            liftTicks_ = 0;
            To(Step::Lift);
            return Status::Working;
        }
        return Status::Working;
    case Step::Lift: {
        if (void* ctrl = E::GetController(player)) {
            ue_wrap::FRotator view = E::GetControlRotation(ctrl);
            view.Pitch = kLiftViewPitchDeg;
            E::SetControlRotation(ctrl, view);
        }
        ue_wrap::FVector at{}, me{};
        const bool off = E::TryGetActorLocation(box, at) && DG::Dist(at, rest_) >= kHeldOffCm;
        if (!off && ++liftTicks_ < kLiftBoundTicks) return Status::Working;
        if (!off)
            UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] box A stands %.1f cm from where it rested after %d ticks of the raised view: "
                    "carried as it is", who_, static_cast<double>(DG::Dist(at, rest_)), kLiftBoundTicks);
        if (!E::TryGetActorLocation(player, me)) { Invalid(who_, "the pawn's place could not be read"); return Status::Failed; }
        const ue_wrap::FVector goal{me.X + away_.X * kCarryCm, me.Y + away_.Y * kCarryCm, me.Z};
        walk_ = DR::StartBackgroundWalk(goal, kCarryReachCm, kWalkDeadlineS, /*carry=*/true);
        UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] carries box A %.0f cm to (%.0f,%.0f,%.0f)", who_, static_cast<double>(kCarryCm),
                static_cast<double>(goal.X), static_cast<double>(goal.Y), static_cast<double>(goal.Z));
        To(Step::Carry);
        return Status::Working;
    }
    case Step::Carry: {
        const int st = walk_ ? walk_->state.load() : 2;
        if (st == 0) {
            Wait(who_, "the carry walk");
            if (Expired()) { Invalid(who_, "the carry walk never ended"); return Status::Failed; }
            return Status::Working;
        }
        walk_.reset();
        if (st != 1) { Invalid(who_, "the carry walk did not arrive"); return Status::Failed; }
        if (!DR::CallOnPlayer(player, L"dropGrabObject")) { Invalid(who_, "dropGrabObject did not run"); return Status::Failed; }
        UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] box A is let go (a plain release)", who_);
        To(Step::Done);
        return Status::Done;
    }
    default:
        return Status::Done;
    }
}

}  // namespace coop::dev::constraint_join_drill::detail
