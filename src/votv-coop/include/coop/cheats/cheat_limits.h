// coop/cheats/cheat_limits.h -- the bound a player's cheat requests keep, in one place. The host's judge draws from a
// bucket of this size per sender (coop/cheats/cheat_request), and a client's menu keeps a bucket of its own that
// refills a little slower, so an honest client stops offering a request before the host would drop it.

#pragma once

#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::cheats {

// A request costs one token, and a request for props costs one per prop. The bucket holds the largest request
// (kCheatMaxCount props) and refills at three tokens a second, so a held button costs the host nothing past it and
// the props a player can spawn in a minute are bounded, however the requests are cut.
inline constexpr coop::net::IntentBudget kRequestBudget{static_cast<float>(coop::net::kCheatMaxCount), 3.0f};

// The client's mirror of that bucket: the same size, refilled at nine tenths of the host's rate. The mirror refills from
// the time a request is SENT and the host's from the time it ARRIVES, so a client that sends the instant its mirror
// allows it can land a little early on the host; the slower refill keeps it behind the host's bucket by more than the
// network's jitter, and an honest client never meets a host drop.
inline constexpr coop::net::IntentBudget kMirrorBudget{kRequestBudget.burst, kRequestBudget.perSecond * 0.9f};

// What a request costs the bucket: `count` for SpawnItem (clamped to what the bucket can hold), one for the rest.
inline float RequestCost(coop::net::CheatOp op, int count) {
    if (op != coop::net::CheatOp::SpawnItem) return 1.0f;
    return static_cast<float>(count < 1 ? 1 : count > coop::net::kCheatMaxCount ? coop::net::kCheatMaxCount : count);
}

}  // namespace coop::cheats
