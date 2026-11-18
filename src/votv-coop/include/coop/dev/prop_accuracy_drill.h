// coop/dev/prop_accuracy_drill.h -- [dev] a client handles props the ways players do, for the drift census to measure
// how far each peer's copies end up from the host's (VOTVCOOP_PROP_ACC=1 on every peer, with VOTVCOOP_PROP_DRIFT=1).
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::prop_accuracy_drill {

// The dev lanes' tick. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::prop_accuracy_drill
