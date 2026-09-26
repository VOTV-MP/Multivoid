// coop/creatures/kerfus_follow.h -- a Kerfus a client turned on follows that client (HOST).
//
// The Kerfus's brain runs on the host alone (kerfus_brain), and it reads the player as player 0: its path
// goal targetActor() returns GetPlayerPawn(0) when it has no valid task and is not possessed (bp_cfg
// targetActor, block @353), and its tick and checkJump judge its approach, its stop and its jump by four
// GetPlayerCharacter(0) reads (ubergraph @3327, @4171, @4442, @11671). On the host player 0 is the host, so
// a Kerfus a client turned on drove to the host. Principle 6: the host records who turned it on -- the slot
// whose KerfusIntent pressed it, else the host -- in served_player, whose native seams answer those five
// reads with that player's puppet; the Blueprint's own branches do the rest, a task's goal and the
// possessed branch's null included. A record lives while its Kerfus is on, a haunting's possess drive
// included, and goes once it is off (pressed off, out of energy, the possess arrived) or its presser
// leaves. Nothing to replay at a join: the host's drive stream carries where it goes. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::kerfus_follow {

// Keep the session: a client never records a presser. Game thread.
void Install(coop::net::Session* session);

// Drop the record of a Kerfus that is off or gone, and its served-player record with it. Every pump tick;
// a loop over the Kerfuses a client turned on, none on a client. Game thread.
void Tick();

// HOST: a press of the Kerfus's on/off verb, by `slot` (0 = the host's own), at the verb's entry,
// before the body flips `active`: a press that turns it on names who it follows. Game thread.
void OnToggle(void* kerfus, uint8_t slot);

// A peer left: the Kerfuses it turned on follow the host again.
void OnPeerLeft(uint8_t slot);

// Session end: every record goes (served_player drops its own).
void OnDisconnect();

}  // namespace coop::kerfus_follow
