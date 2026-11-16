// coop/player/slot_count.h -- how many player slots a session has, and the two sentinel ids. A leaf
// header (only <cstdint>) so the net layer and every per-slot container can size themselves from the
// one constant without pulling in the engine-coupled players registry.

#pragma once

#include <cstdint>

namespace coop::players {

// Peer session ids: the host is 0, clients count up from 1. Each peer has one local and up to
// the maximum minus one puppets; the maximum covers a host and three clients.
inline constexpr uint8_t kMaxPeers       = 4;
inline constexpr uint8_t kPeerIdHost     = 0;
inline constexpr uint8_t kPeerIdUnknown  = 0xFF;  // not in registry

}  // namespace coop::players
