// coop/dev/atv_riders_join_hold.cpp -- see coop/dev/atv_riders_join_hold.h.

#include "coop/dev/atv_riders_join_hold.h"

#include "coop/config/config.h"
#include "coop/dev/atv_riders_arm.h"

#include <atomic>

namespace coop::dev::atv_riders_join_hold {
namespace {

// Resolved on the first ask: both holds start held in the drill's rejoined life, free everywhere else.
struct Holds {
    std::atomic<bool> birth{false};
    std::atomic<bool> bind{false};
    Holds() {
        const bool on = RejoinedLife() && !coop::dev::atv_riders_arm::Is("off");
        birth.store(on);
        bind.store(on);
    }
};

Holds& H() {
    static Holds h;
    return h;
}

}  // namespace

bool RejoinedLife() {
    static const bool rejoined = coop::config::ReadEnv("VOTVCOOP_ATV_RIDERS_REJOIN") == "1";
    return rejoined;
}

bool BirthHeld(uint8_t slot) { return slot == kDriverSlot && H().birth.load(std::memory_order_acquire); }

bool BindHeld() { return H().bind.load(std::memory_order_acquire); }

void ReleaseBirth() { H().birth.store(false, std::memory_order_release); }

void ReleaseBind() { H().bind.store(false, std::memory_order_release); }

}  // namespace coop::dev::atv_riders_join_hold
