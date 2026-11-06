// coop/dev/kerfur_seat_probe.h -- [dev] which seat sees a kerfur Omega radial command, on each route.
//
// A real press reaches the Omega's verb through the radial dispatch (mainPlayer_C::useSelectedAction, an interface
// call into kerfurOmega_C::actionName); a reflected call (a drill) through ProcessEvent. The script gate sees both;
// this probe says which watch fires. Two PRE watches, observe only (always Run):
//   [KERFUR-SEAT] role=<host|client|solo> actionName obj=<Omega> action=<the name argument> caller=<Class.fn|none> ours=<0|1>
//   [KERFUR-SEAT] role=<r> useSelectedAction self=<player> local=<0|1> looked=<Class:name|?> sel=<actionIndex|?> release=<0|1|?>
//   [KERFUR-SEAT] role=<r> get_reports gates obj=<Omega> state=<n> hasFloppy=<0|1> animation=<0|1> server=<valid|null> remoteControl=<0|1> caller=<..> ours=<0|1>
//   [KERFUR-SEAT] role=<r> get_reports after obj=<Omega> state=<n> hasFloppy=<0|1>   (the next tick; `gone` if freed)
//   [KERFUR-SEAT] watch '<fn>' live           (or: NOT live: <why>)
//   [KERFUR-SEAT] session end -- actionName=<n> useSelectedAction=<n>
// caller=none is a call with no calling Blueprint frame: the ProcessEvent route. The first 200 lines of each watch
// are printed, the rest counted (the gates lines share the cap); the gates are the Omega's own properties read by
// reflection, a missing one printed ? and said once. Ini kerfur_seat_probe=1 or VOTVCOOP_KERFUR_SEAT_PROBE=1, on every peer to be read.
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
