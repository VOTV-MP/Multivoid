// coop/dev/kerfur_seat_probe.h -- [dev] which seat sees a kerfur Omega radial command, on each route.
//
// A real player press reaches the Omega's verb through the radial dispatch (mainPlayer_C::useSelectedAction, an
// interface call into kerfurOmega_C::actionName); a reflected call (K::RunActionName, a drill) reaches it through
// ProcessEvent. The script gate sees every Blueprint body on either route; this probe says which of its two watches
// fires, so the seat that gates a command is picked from a measurement. Two PRE watches, observe only (always Run):
//   [KERFUR-SEAT] role=<host|client|solo> actionName obj=<Omega> action=<the name argument> caller=<Class.fn|none> ours=<0|1>
//   [KERFUR-SEAT] role=<r> useSelectedAction self=<player> local=<0|1> looked=<Class:name|?> sel=<actionIndex|?> release=<0|1|?>
//   [KERFUR-SEAT] watch '<fn>' live            (or: NOT live: <why>)
//   [KERFUR-SEAT] session end -- actionName=<n> useSelectedAction=<n>
// caller=none is a call with no calling Blueprint frame: the ProcessEvent route. The first 200 lines of each watch
// are printed, the rest counted. Ini kerfur_seat_probe=1 or VOTVCOOP_KERFUR_SEAT_PROBE=1, on every peer to be read.
// Off, nothing is registered. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_seat_probe {

// Cache the session, for the role; register the two watches while the row is on, retried each pump tick until each
// settles. A latched read when off.
void Install(coop::net::Session* session);

// Session end: the counters are said and cleared.
void OnDisconnect();

}  // namespace coop::dev::kerfur_seat_probe
