// coop/dev/cheat_drill_raw.h -- the cheat drill's hostile half: raw CheatRequest payloads a client's own panel calls
// cannot make (a bad op, a name without its end, a count the wire forbids, an amount past the bound, a target nobody
// holds, a request the host must refuse), sent as they are, with the answer each must get. Dev-gated: nothing is sent
// unless the cheat drill is on. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstddef>

namespace coop::net { class Session; }

namespace coop::dev::cheat_drill {

struct RawCase {
    const char* what;                     // for the drill's line
    coop::net::CheatRequestPayload p;     // sent whole
    coop::net::CheatReason want;          // the host's answer
};

// The hostile cases in the order they are sent, and how many.
size_t RawCaseCount();
const RawCase& RawCaseAt(size_t i);

// A request for `count` props named `name` in front of the sender, built as the wire carries it, with `id` as its id.
coop::net::CheatRequestPayload RawSpawn(uint32_t id, const char* name, int count);

// Send `p` to the host as a CheatRequest exactly as given, with none of the client's own checks (its rate mirror, the
// clamps its panel path makes). False unless the cheat drill is on and this is a client.
bool SendRawForDrill(coop::net::Session& s, const coop::net::CheatRequestPayload& p);

}  // namespace coop::dev::cheat_drill
