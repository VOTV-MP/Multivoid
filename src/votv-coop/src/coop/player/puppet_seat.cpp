// coop/player/puppet_seat.cpp -- see coop/player/puppet_seat.h.

#include "coop/player/puppet_seat.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_passenger_seat.h"
#include "ue_wrap/engine/engine.h"

namespace coop {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

}  // namespace

bool PuppetSeat::Set(void* puppet, void* atv, bool passenger) {
    if (atv == atv_ && (!atv || passenger == passenger_)) return false;
    Release(puppet);
    if (!atv || !R::IsLive(atv)) return true;
    atv_ = atv;
    atvIdx_ = R::InternalIndexOf(atv);
    passenger_ = passenger;
    E::SetActorEnableCollision(puppet, false);
    // Placed once, then attached where it stands: the ATV's own movement carries the body within the frame. Placed by
    // the pump alone it was set from the ATV's pose a frame before the ATV moved, and trailed the seat by up to 27 cm
    // at full speed, catching up as the ATV slowed.
    attached_ = Drive(puppet) && E::AttachActorToActorKeepWorld(puppet, atv, /*weld*/false);
    if (!attached_) UE_LOGW("puppet_seat: the body did not attach to ATV %p -- it is placed each tick instead", atv);
    if (!ue_wrap::puppet::SetOnAtv(puppet, true))
        UE_LOGW("puppet_seat: the puppet's AnimBP took no seat pose -- the driver shows standing in the seat");
    UE_LOGI("puppet_seat: a remote %s's body sits on ATV %p", passenger ? "passenger" : "driver", atv);
    return true;
}

bool PuppetSeat::Drive(void* puppet) {
    if (!atv_) return false;
    if (attached_) {
        // The ATV carries the body; a world write here would set it back to where the ATV stood before its move.
        if (!R::IsLiveByIndex(atv_, atvIdx_)) {
            Release(puppet);
            return false;
        }
        ue_wrap::puppet::DriveCharacterMovement(puppet, ue_wrap::FVector{}, false);
        ue_wrap::puppet::SetOnAtv(puppet, true);
        return true;
    }
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    const bool placed = passenger_ ? ue_wrap::atv_passenger_seat::BodyTransform(atv_, loc, rot)
                                   : ue_wrap::atv::GetSeatTransform(atv_, loc, rot);
    if (!R::IsLiveByIndex(atv_, atvIdx_) || !placed) {
        Release(puppet);
        return false;
    }
    E::SetActorLocation(puppet, loc);
    E::SetActorRotation(puppet, rot);
    ue_wrap::puppet::DriveCharacterMovement(puppet, ue_wrap::FVector{}, false);
    ue_wrap::puppet::SetOnAtv(puppet, true);   // the AnimBP keeps it; written again so a re-skin's new instance has it
    return true;
}

void PuppetSeat::Release(void* puppet) {
    if (!atv_) return;
    const bool wasAttached = attached_;
    Forget();
    if (!puppet || !R::IsLive(puppet)) return;
    if (wasAttached) E::DetachActorFromParent(puppet);
    ue_wrap::puppet::SetOnAtv(puppet, false);
    E::SetActorEnableCollision(puppet, true);
    UE_LOGI("puppet_seat: a remote rider's body leaves its seat");
}

}  // namespace coop
