// coop/world/event_box_authority.h -- an event box whose walk-in makes shared props fires on the host alone.
//
// A story event's trigger_box_N_C runs its targets when a player of its class list walks in. The host's own copy sees a
// client's puppet, a mainPlayer_C, as it sees its own player, so a client's walk-in already runs the host's box and
// what it makes reaches every peer. A box whose row is replayed on clients was armed on the client too, and its walk-in
// ran the client's copy as well: the dropped Ariral gun (TB_event_arirToe) then stood on that client twice, the host's
// and its own. CLIENT: the overlap of each listed box is refused, so only the host's box runs. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::event_box_authority {

void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once per pump tick.
void Tick();

void OnDisconnect();

}  // namespace coop::event_box_authority
