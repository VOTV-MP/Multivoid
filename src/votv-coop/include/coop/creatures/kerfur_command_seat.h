// coop/creatures/kerfur_command_seat.h -- the one seat where every route of an Omega's radial command is decided.
//
// A player's radial pick reaches kerfurOmega_C::actionName as an interface call from mainPlayer_C::useSelectedAction,
// a drill's or the host's replay as a reflected one; the script gate's PRE watch on actionName sees both (measured:
// the radial pick with caller useSelectedAction, the reflected call with none), which the ProcessEvent detour never
// saw for the first. At the watch, a command in the relayed set (kerfur_command::CommandFromActionName) is cancelled
// and recorded by kerfur_command, which sends it to the host from a client and, on the host, hands an Omega that serves
// a client back to the host's own player; the host's press on an Omega nobody serves runs untouched, as does every
// other action (turn_off, take_object, pat, equipment). The host's own replay of a
// client's command runs through the watch under kerfur_command's host-exec scope and is let through. The seat never
// reads whether the call came from our code, and a client never asks whether the Omega is served: a client authors no
// Omega's command, so every relayed command on every Omega is the host's.
//
// Principle 7: gameplay module; engine access through ue_wrap/core only. Page: docs/npcs-and-kerfur.md.

#pragma once

namespace coop::net { class Session; }

namespace coop::kerfur_command_seat {

// Cache the session (every call, so a reconnect lands) and register the actionName watch once, both roles; a retry
// pump settles it, and a watch that is refused or settles dead is said once, with what it costs. Game thread.
void Install(coop::net::Session* session);

// Net disconnect: drop the cached session. The watch stays registered for the process.
void OnDisconnect();

}  // namespace coop::kerfur_command_seat
