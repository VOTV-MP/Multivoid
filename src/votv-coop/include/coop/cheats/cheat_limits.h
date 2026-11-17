// coop/cheats/cheat_limits.h -- the bound a player's cheat requests keep, in one place. The host's judge takes from a
// bucket of this size per sender (coop/cheats/cheat_request), and a client's menu keeps a bucket of the same size of
// its own, so an honest client stops offering a request before the host would drop it.

#pragma once

#include "coop/net/intent_bucket.h"

namespace coop::cheats {

// A burst of eight (a row of preset presses), then three a second. A button a person can hold down costs the host
// nothing past it.
inline constexpr coop::net::IntentBudget kRequestBudget{8.0f, 3.0f};

}  // namespace coop::cheats
