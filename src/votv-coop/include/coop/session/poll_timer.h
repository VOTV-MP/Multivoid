// coop/session/poll_timer.h -- a lane's fixed-rate poll on the steady clock. Game thread.

#pragma once

#include <chrono>

namespace coop::session {

class PollTimer {
public:
    using Clock = std::chrono::steady_clock;

    explicit constexpr PollTimer(std::chrono::milliseconds period) : period_(period) {}

    // Whether a poll is due at `now`; true starts the next period.
    bool Due(Clock::time_point now) {
        if (now < next_) return false;
        next_ = now + period_;
        return true;
    }

private:
    std::chrono::milliseconds period_;
    Clock::time_point next_{};
};

}  // namespace coop::session
