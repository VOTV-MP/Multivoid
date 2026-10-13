// coop/net/refusal_tally.cpp -- see coop/net/refusal_tally.h.

#include "coop/net/refusal_tally.h"

#include "ue_wrap/core/log.h"

namespace coop::net {

void RefusalTally::Note(std::uint8_t slot, const char* why, Clock::time_point now) {
    if (open_ && now - openedAt_ < kWindow) {
        ++more_;
        lastSlot_ = slot;
        lastWhy_ = why;
        return;
    }
    Flush(now);
    UE_LOGW("%s: refused a row from slot %u -- %s", lane_, static_cast<unsigned>(slot), why);
    open_ = true;
    openedAt_ = now;
    more_ = 0;
}

void RefusalTally::Flush(Clock::time_point now) {
    if (!open_ || now - openedAt_ < kWindow) return;
    if (more_ > 0)
        UE_LOGW("%s: refused %u more row(s) in the %lld s after it -- the last from slot %u: %s", lane_,
                more_, static_cast<long long>(kWindow.count()), static_cast<unsigned>(lastSlot_), lastWhy_);
    open_ = false;
    more_ = 0;
}

void RefusalTally::Clear() {
    // The session ends: an open window's sum is said now, since no later tick will close it.
    if (open_ && more_ > 0)
        UE_LOGW("%s: refused %u more row(s) before the session ended -- the last from slot %u: %s", lane_,
                more_, static_cast<unsigned>(lastSlot_), lastWhy_);
    open_ = false;
    more_ = 0;
}

}  // namespace coop::net
