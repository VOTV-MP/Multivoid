// coop/dev/bucket_drill.h -- [dev] one player fills a mop bucket, empties it, makes concrete in it and takes the
// concrete out with the trowel, while every peer logs every bucket's water and concrete (VOTVCOOP_BUCKET_DRILL=1 on
// every peer; VOTVCOOP_BUCKET_ACTOR=host|client picks who acts, client by default).
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bucket_drill {

// The dev lanes' tick. A latched read when the drill is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::bucket_drill
