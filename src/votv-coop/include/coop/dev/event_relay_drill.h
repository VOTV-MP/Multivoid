// coop/dev/event_relay_drill.h -- drill: the host's creature event reaches a client's event count, once, through the
// relay (ini event_relay_drill=off|on / env VOTVCOOP_EVENT_RELAY_DRILL; BOTH peers, one run, one rejoin).
//
// The host spawns a krampus 15 m above itself, inert, once a client's join is over; its BeginPlay registers one
// event. The client's evidence is the krampus mirror's own copies in the gamemode's event senders: 1 when the relay
// applied the host's count alone, 0 with no relay, 2 when the mirror's BeginPlay registration was not retracted. The
// rejoined client reads the same, and its chat line tells the host to destroy the krampus; the client then sees its
// count fall by one and the host sees its own return to where it began. Every verdict line carries the tag EVR in
// brackets, then PASS or FAIL, the step and the role; a step's expiry is a FAIL line, never a silence. The step list
// is in coop/dev/event_relay_drill.cpp. Run: the rig's smoke with event_relay_drill=on on both peers, --host-fresh
// --rejoin, the client's read PASS as the rejoin marker and its done PASS as the done marker.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::event_relay_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session. Called from the dev install fanout; no-op when off.
void Install(coop::net::Session* session);

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick();

// The session ended: the client's steps back to the first. The host's spawn, krampus and counts stay with its
// process, which lives through the client's leave.
void OnDisconnect();

}  // namespace coop::dev::event_relay_drill
