// coop/dev/clean_drill.h -- [dev] one player cleans grime of each kind, bleeds and cleans its blood, dips the mop and
// throws a bucket's water out, while every peer logs the grime and the sounds it hears (VOTVCOOP_CLEAN_DRILL=1 on every
// peer; VOTVCOOP_CLEAN_ACTOR=host|client picks who acts, client by default).
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::clean_drill {

// The dev lanes' tick. A latched read when the drill is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::clean_drill
