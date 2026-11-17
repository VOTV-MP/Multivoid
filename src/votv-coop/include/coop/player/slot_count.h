// coop/player/slot_count.h -- how many player slots a session has, and the two sentinel ids. A leaf
// header (only <cstdint>) so the net layer and every per-slot container can size themselves from the
// one constant without pulling in the engine-coupled players registry.

#pragma once

#include <cstdint>

namespace coop::players {

// Peer session ids: the host is 0, clients count up from 1. Each peer has one local and up to
// the maximum minus one puppets; the maximum covers a host and fifteen clients. This is the hard
// ceiling every peer agrees on (the element bands and the wire's per-slot arrays are sized by
// it); how many seats a host opens is its `net.max_players` row, never above this.
inline constexpr uint8_t kMaxPeers       = 16;
inline constexpr uint8_t kPeerIdHost     = 0;
inline constexpr uint8_t kPeerIdUnknown  = 0xFF;  // not in registry

}  // namespace coop::players
