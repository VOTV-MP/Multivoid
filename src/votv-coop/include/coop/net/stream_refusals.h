// coop/net/stream_refusals.h -- the pose packets a receiver refuses, counted per origin slot and said
// once per streak. A refusal that lasts is a failure the pose store once kept silent: a peer's stream
// stopped applying for minutes and no line said why, so a stale sequence could not be told from a
// failed validation or a lost datagram. One stray reordered datagram is not a streak; sixty in a row,
// about a second of poses, is, and its end is said too.
#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

#include <array>
#include <cstdint>

namespace coop::net {

class StreamRefusals {
public:
    static constexpr int kSlots = coop::players::kMaxPeers;
    enum class Why : std::uint8_t { StaleSequence, FailedValidation, OtherOccupancy };

    // Under the session's remote-state lock.
    void Refused(int slot, Why why, std::uint32_t seq, std::uint32_t lastAccepted);
    void Accepted(int slot);
    void Clear(int slot);

private:
    struct Streak {
        std::uint32_t count = 0;
        std::uint64_t startMs = 0;
        Why why = Why::StaleSequence;
    };
    std::array<Streak, kSlots> streaks_{};
};

}  // namespace coop::net
