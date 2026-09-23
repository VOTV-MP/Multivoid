// coop/dev/rehost_rejoin.h -- drill: a client that stays running joins its host again (ini
// rehost_rejoin=1 / env VOTVCOOP_REHOST_REJOIN, off by default; CLIENT).
//
// The rig's host-restart arm kills the host under a joined client and launches it again. The client
// loses its host, flees to the menu and keeps its process, so its session object, its stream state
// and its observers carry into the next session -- the case a relaunched client never reaches. Once
// the new host is up the rig drops multivoid-rejoin.trigger beside the game exe; at the menu, with
// no session running and no join in flight, the drill deletes the file and dials net.peer:net.port
// through the browser's own direct connect, so the second join is the one a player makes by hand.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::rehost_rejoin {

// Look for the trigger, at most once a second, and dial. Every tick without a session. Game thread.
void Tick(const coop::net::Session& session);

}  // namespace coop::dev::rehost_rejoin
