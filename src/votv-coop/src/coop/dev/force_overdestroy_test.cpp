// coop/dev/force_overdestroy_test.cpp -- see header.

#include "coop/dev/force_overdestroy_test.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

namespace coop::dev::force_overdestroy_test {

bool HostSkipChipPileExpression() {
    static const bool s = [] {
        const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::force_chippile_unclaim);
        if (on) {
            UE_LOGW("force_overdestroy_test: ARMED -- HOST will SKIP expressing ALL chipPiles this "
                    "session (injecting the over-destroy to PROVE the Phase 0 floor). "
                    "The joiner's seeded natives stay UNCLAIMED -> its sweep dooms them; the floor "
                    "binary KEEPs them, a no-floor baseline WIPES them.");
        }
        return on;
    }();
    return s;
}

bool FloorDisabledForTest() {
    static const bool s = [] {
        const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::disable_completeness_floor);
        if (on) {
            UE_LOGW("force_overdestroy_test: completeness FLOOR DISABLED for test -- the claim sweep "
                    "will behave like a no-floor baseline (the BEFORE half). With force_chippile_unclaim "
                    "set this WIPES the unclaimed piles; the same binary with this OFF KEEPs them.");
        }
        return on;
    }();
    return s;
}

bool BoundCountDisabledForTest() {
    static const bool s = [] {
        const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::sweep_bound_count_off);
        if (on) {
            UE_LOGW("force_overdestroy_test: the sweep's bound-native count is OFF for test -- a bound mirror "
                    "native no longer counts as claimed or in the valve's universe, so the completeness floor keeps the orphan piles of a "
                    "class the host still has (the red arm of the orphan drill).");
        }
        return on;
    }();
    return s;
}

bool FreshUniverseDisabledForTest() {
    static const bool s = [] {
        const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::sweep_fresh_universe_off);
        if (on) {
            UE_LOGW("force_overdestroy_test: the sweep's trash walk is OFF for test -- no unbound native pile or "
                    "clump is found, so every orphan survives the join (the red arm of the orphan drill).");
        }
        return on;
    }();
    return s;
}

}  // namespace coop::dev::force_overdestroy_test
