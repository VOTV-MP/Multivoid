// coop/dev/force_overdestroy_test.cpp -- see header.

#include "coop/dev/force_overdestroy_test.h"

#include "coop/ini_config.h"
#include "ue_wrap/log.h"

namespace coop::dev::force_overdestroy_test {

bool HostSkipChipPileExpression() {
    static const bool s = [] {
        const bool on = coop::ini_config::IsIniKeyTrue("force_chippile_unclaim");
        if (on) {
            UE_LOGW("force_overdestroy_test: ARMED -- HOST will SKIP expressing ALL chipPiles this "
                    "session (injecting the docs/piles/10 over-destroy to PROVE the Phase 0 floor). "
                    "The joiner's seeded natives stay UNCLAIMED -> its sweep dooms them; the floor "
                    "binary KEEPs them, a no-floor baseline WIPES them.");
        }
        return on;
    }();
    return s;
}

}  // namespace coop::dev::force_overdestroy_test
