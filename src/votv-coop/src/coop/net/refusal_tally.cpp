// coop/net/refusal_tally.cpp -- see coop/net/refusal_tally.h.

#include "coop/net/refusal_tally.h"

#include "ue_wrap/core/log.h"

#include <cstdio>

namespace coop::net {
namespace {

// "slot 1 x40, slot 2 x3" for the senders with a count; the last bucket reads "other x2".
void BySender(const std::uint32_t (&perSlot)[RefusalTally::kSlots], char (&out)[160]) {
    out[0] = '\0';
    size_t n = 0;
    for (int i = 0; i < RefusalTally::kSlots && n + 24 < sizeof(out); ++i) {
        if (perSlot[i] == 0) continue;
        const int w = i == RefusalTally::kSlots - 1
                          ? std::snprintf(out + n, sizeof(out) - n, "%sother x%u", n ? ", " : "", perSlot[i])
                              : std::snprintf(out + n, sizeof(out) - n, "%sslot %d x%u", n ? ", " : "", i, perSlot[i]);
        if (w > 0) n += static_cast<size_t>(w);
    }
}

}  // namespace

void RefusalTally::Note(std::uint8_t slot, const char* why, Clock::time_point now) {
    if (open_ && now - openedAt_ < kWindow) {
        ++more_;
        ++perSlot_[slot < kSlots - 1 ? slot : kSlots - 1];
        lastSlot_ = slot;
        lastWhy_ = why;
        return;
    }
    Flush(now);
    UE_LOGW("%s: refused a row from slot %u -- %s", lane_, static_cast<unsigned>(slot), why);
    open_ = true;
    openedAt_ = now;
    more_ = 0;
    for (auto& c : perSlot_) c = 0;
}

void RefusalTally::Flush(Clock::time_point now) {
    if (!open_ || now - openedAt_ < kWindow) return;
    if (more_ > 0) {
        char by[160];
        BySender(perSlot_, by);
        UE_LOGW("%s: refused %u more row(s) in the %lld s after it (%s) -- the last from slot %u: %s", lane_,
                more_, static_cast<long long>(kWindow.count()), by, static_cast<unsigned>(lastSlot_), lastWhy_);
    }
    open_ = false;
    more_ = 0;
}

void RefusalTally::Clear() {
    // The session ends: an open window's sum is said now, since no later tick will close it.
    if (open_ && more_ > 0) {
        char by[160];
        BySender(perSlot_, by);
        UE_LOGW("%s: refused %u more row(s) before the session ended (%s) -- the last from slot %u: %s", lane_,
                more_, by, static_cast<unsigned>(lastSlot_), lastWhy_);
    }
    open_ = false;
    more_ = 0;
}

}  // namespace coop::net
