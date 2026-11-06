// coop/player/puppet_seat.cpp -- see coop/player/puppet_seat.h.

#include "coop/player/puppet_seat.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

namespace coop {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

}  // namespace

bool PuppetSeat::Set(void* puppet, void* atv) {
    if (atv == atv_) return false;
    Release(puppet);
    if (!atv || !R::IsLive(atv)) return true;
    atv_ = atv;
    atvIdx_ = R::InternalIndexOf(atv);
    E::SetActorEnableCollision(puppet, false);
    if (!ue_wrap::puppet::SetOnAtv(puppet, true))
        UE_LOGW("puppet_seat: the puppet's AnimBP took no seat pose -- the driver shows standing in the seat");
    UE_LOGI("puppet_seat: a remote driver's body sits on ATV %p", atv);
    return true;
}

bool PuppetSeat::Drive(void* puppet) {
    if (!atv_) return false;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!R::IsLiveByIndex(atv_, atvIdx_) || !ue_wrap::atv::GetSeatTransform(atv_, loc, rot)) {
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
    Forget();
    if (!puppet || !R::IsLive(puppet)) return;
    ue_wrap::puppet::SetOnAtv(puppet, false);
    E::SetActorEnableCollision(puppet, true);
    UE_LOGI("puppet_seat: a remote driver's body leaves its seat");
}

}  // namespace coop
