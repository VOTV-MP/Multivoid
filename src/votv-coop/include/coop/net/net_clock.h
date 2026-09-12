// coop/net/net_clock.h -- the net layer's one monotonic millisecond clock. Every age the net
// layer keeps (a pending entry's park time, a source's stamps in the connection cap and the
// password-guess bound) is measured against this and nothing else, so two sites that feed one
// table agree on what time it is. steady_clock, not GetTickCount64: no <windows.h> in net
// headers, and a monotonic stamp is right for an age (a wall-clock jump must neither free nor
// hold a seat).

#pragma once

#include <chrono>
#include <cstdint>

namespace coop::net {

inline uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace coop::net
