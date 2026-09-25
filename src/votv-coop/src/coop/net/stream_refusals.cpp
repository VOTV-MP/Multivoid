// coop/net/stream_refusals.cpp -- see coop/net/stream_refusals.h.
#include "coop/net/stream_refusals.h"

#include "ue_wrap/core/log.h"

#include <chrono>

namespace coop::net {

namespace {

constexpr std::uint32_t kStreak = 60;

std::uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* Text(StreamRefusals::Why why) {
    switch (why) {
        case StreamRefusals::Why::StaleSequence:    return "a sequence at or below the last accepted";
        case StreamRefusals::Why::FailedValidation: return "a pose that failed validation";
        case StreamRefusals::Why::OtherOccupancy:   return "an occupancy of the slot its roster does not name";
    }
    return "?";
}

bool Valid(int slot) { return slot >= 0 && slot < StreamRefusals::kSlots; }

}  // namespace

void StreamRefusals::Refused(int slot, Why why, std::uint32_t seq, std::uint32_t lastAccepted) {
    if (!Valid(slot)) return;
    Streak& s = streaks_[slot];
    if (s.count == 0) s.startMs = NowMs();
    s.why = why;
    if (++s.count == kStreak)
        UE_LOGW("net: slot %d: %u pose packets refused in a row, the latest for %s (seq %u, last "
                "accepted %u) -- still refusing", slot, s.count, Text(why), seq, lastAccepted);
}

void StreamRefusals::Accepted(int slot) {
    if (!Valid(slot)) return;
    Streak& s = streaks_[slot];
    if (s.count >= kStreak)
        UE_LOGI("net: slot %d: the refusal streak ended after %u packets over %llu ms (the last for %s)",
                slot, s.count, static_cast<unsigned long long>(NowMs() - s.startMs), Text(s.why));
    s.count = 0;
}

void StreamRefusals::Clear(int slot) {
    if (!Valid(slot)) return;
    const Streak& s = streaks_[slot];
    if (s.count >= kStreak)
        UE_LOGI("net: slot %d: the refusal streak ended with its streams reset, after %u packets over %llu ms "
                "(the last for %s)", slot, s.count,
                static_cast<unsigned long long>(NowMs() - s.startMs), Text(s.why));
    streaks_[slot] = Streak{};
}

}  // namespace coop::net
