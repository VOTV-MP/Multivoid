// coop/gamefix/body_guard.cpp -- see coop/gamefix/body_guard.h.

#include "coop/gamefix/body_guard.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_fingerprint.h"

namespace coop::gamefix {

namespace R = ue_wrap::reflection;

bool BodyGuard::Holds(void* function) {
    if (!function) return false;
    if (function == fn_ && R::IsLiveByIndex(fn_, fnIdx_) && R::SlotSerial(fnIdx_) == fnSerial_) return holds_;
    fn_ = function;
    fnIdx_ = R::InternalIndexOf(function);
    fnSerial_ = R::AllocateSlotSerial(fnIdx_);
    const ue_wrap::script_fingerprint::Result fp = ue_wrap::script_fingerprint::Of(function);
    holds_ = fp.ok && fp.bodyHash == want_;
    if (holds_)
        UE_LOGI("gamefix[%s]: the game's function is the body this fix corrects -- the fix is on", what_);
    else
        UE_LOGW("gamefix[%s]: the game's function is another body (hash %016llx, the fix corrects %016llx%s%s) "
                "-- the fix stays off",
                what_, static_cast<unsigned long long>(fp.bodyHash), static_cast<unsigned long long>(want_),
                fp.ok ? "" : "; not decoded: ", fp.ok ? "" : fp.problem.c_str());
    return holds_;
}

}  // namespace coop::gamefix
