// coop/net/refusal_tally.h -- one receive lane's log of the refusals of what its peers sent. The first refusal
// after a quiet spell is said at once, so a log with no such line is a lane that refused nothing; the rest of a
// burst is summed into one line per window, so a sender cannot make the log the cost. MTA bounds its unknown-event
// error the same way, at most one line per gap (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2696-2699);
// its refusal of a known event a client may not trigger logs each time (:2683-2691). What a sender may send at all
// is the receive edge's bound (receive_budget.h), not a count kept here. One thread per instance: the lanes' on the
// game thread, the receive edge's on the net thread.
#pragma once

#include <chrono>
#include <cstdint>

namespace coop::net {

class RefusalTally {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kWindow{10};

    // `lane` names the lane in every line ("signal_sync"); a static string.
    explicit RefusalTally(const char* lane) : lane_(lane) {}

    // One refusal of a row from `slot`, for the reason `why` (a static string).
    void Note(std::uint8_t slot, const char* why, Clock::time_point now);
    // Closes a window past its span: the burst's summed line, if more than its first refusal came.
    // Cheap when no window is open; call it from the lane's tick.
    void Flush(Clock::time_point now);

    void Clear();

private:
    const char* lane_;
    bool open_ = false;
    Clock::time_point openedAt_{};
    std::uint32_t more_ = 0;       // refusals in the open window after its first
    std::uint8_t lastSlot_ = 0;
    const char* lastWhy_ = "";
};

}  // namespace coop::net
