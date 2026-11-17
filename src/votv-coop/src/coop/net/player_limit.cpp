// coop/net/player_limit.cpp -- see coop/net/player_limit.h.

#include "coop/net/player_limit.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/slot_count.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdlib>
#include <string>

namespace coop::net::player_limit {
namespace {

// 0 until the first read: Max() answers the row's default then.
std::atomic<int> g_max{0};

// A token of the row as a seat count, or 0 for one that is not a whole number in 1..kMaxPeers. The
// registry's enum reader already refuses a token outside the row's list, so 0 is not reached by a
// value a person typed; it guards a list edited without the ceiling.
int SeatsOf(const char* token) {
    char* end = nullptr;
    const long n = std::strtol(token, &end, 10);
    if (end == token || *end != '\0' || n < 1 || n > coop::players::kMaxPeers) return 0;
    return static_cast<int>(n);
}

int RowDefault() {
    const int n = SeatsOf(::coop::config_registry::rows::net_max_players.row->defS);
    return n > 0 ? n : 1;
}

void Apply() {
    const std::string token = coop::config::ResolveEnum(::coop::config_registry::rows::net_max_players);
    const int n = SeatsOf(token.c_str());
    const int seats = n > 0 ? n : RowDefault();
    const int was = g_max.exchange(seats, std::memory_order_acq_rel);
    if (was != seats)
        UE_LOGI("player_limit: this server seats %d player(s) (net.max_players)", seats);
}

}  // namespace

void Init() {
    coop::config::Subscribe(::coop::config_registry::rows::net_max_players, &Apply);
    Apply();
}

int Max() {
    const int n = g_max.load(std::memory_order_acquire);
    return n > 0 ? n : RowDefault();
}

}  // namespace coop::net::player_limit
