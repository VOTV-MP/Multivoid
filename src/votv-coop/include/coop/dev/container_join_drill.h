// coop/dev/container_join_drill.h -- [dev] the container join drill (ini container_join_drill / env
// VOTVCOOP_CONTAINER_JOIN_DRILL; BOTH peers). An edit made while a joiner is connected but not world-ready has no
// peer to take it: the host's container send is refused and, since it is owed to the connect replay, not re-armed.
// The HOST, the first tick a slot is connected and not world-ready, extracts one item from a world container and
// says `[CONTAINER-JOIN] host fired extract`. The CLIENT, once its join has settled (the phase left idle and came back,
// the container readiness pen empty, the bind wait holding no container slice), sends the host the content hash of
// every world container it holds (coop/dev/container_parity) and says `[CONTAINER-JOIN] client sent parity`. The
// host judges the report: the contents must be equal, the edit must have met a no-ready refusal, and no refusal may
// have been re-armed. `[CONTAINER-JOIN] host PASS slot` per slot, `host DONE` when every slot passed; every
// failure line starts `[CONTAINER-JOIN] host FAIL:`. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_join_drill {

// Once per pump tick; a latched bool read when off.
void Tick(coop::net::Session* session);

// The session ended: a later session's joiners start over.
void OnDisconnect();

}  // namespace coop::dev::container_join_drill
