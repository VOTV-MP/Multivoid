// coop/player/puppet_seat.cpp -- see coop/player/puppet_seat.h.

#include "coop/player/puppet_seat.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

namespace coop {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

// [dev] The riders probe (coop/dev/atv_riders_probe) seats the body with one attach in place of the per-frame writes, to
// measure that the attach follows the ATV. Resolved once; off, every path below is the per-frame seat.
bool ProbeAttach() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::atv_riders_probe) == "on";
    return on;
}

// The probe's one placement: the body at the seat, then attached to the ATV keeping that world pose.
void PlaceAndAttach(void* puppet, void* atv) {
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::atv::GetSeatTransform(atv, loc, rot)) {
        UE_LOGW("puppet_seat: the probe's seat transform does not read -- the body is not placed");
        return;
    }
    E::SetActorLocation(puppet, loc);
    E::SetActorRotation(puppet, rot);
    if (!E::AttachActorToActorKeepWorld(puppet, atv, false))
        UE_LOGW("puppet_seat: the probe's attach of the body to the ATV did not run");
}

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
    if (ProbeAttach()) PlaceAndAttach(puppet, atv);
    UE_LOGI("puppet_seat: a remote driver's body sits on ATV %p", atv);
    return true;
}

bool PuppetSeat::Drive(void* puppet) {
    if (!atv_) return false;
    if (ProbeAttach()) {
        // Seated is the attach: nothing is written, so the stream's writes are skipped and the seat pose is not rewritten.
        if (R::IsLiveByIndex(atv_, atvIdx_)) return true;
        Release(puppet);
        return false;
    }
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
    if (ProbeAttach()) E::DetachActorFromParent(puppet);
    ue_wrap::puppet::SetOnAtv(puppet, false);
    E::SetActorEnableCollision(puppet, true);
    UE_LOGI("puppet_seat: a remote driver's body leaves its seat");
}

}  // namespace coop
