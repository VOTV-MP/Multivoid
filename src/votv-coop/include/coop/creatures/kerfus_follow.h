// coop/creatures/kerfus_follow.h -- a Kerfus a client turned on follows that client (HOST).
//
// The Kerfus's brain runs on the host alone (kerfus_brain), and it follows player 0 twice over:
// targetActor() -- the Blueprint function the path is built to -- returns GetPlayerPawn(0) when the Kerfus
// has no valid task and is not possessed (bp_cfg targetActor, blocks @0 and @353), and its tick and
// checkJump judge its approach, its stop and its jump by four native GetPlayerCharacter(0) reads
// (ubergraph @3327, @4171, @4442, @11671). On the host player 0 is the host, so a Kerfus a client turned
// on drove to the host. Principle 6: the host records who turned it on -- the slot whose KerfusIntent
// pressed it, else the host -- and answers both with that player's puppet: targetActor at the script-body
// gate, whose pre does in that branch alone what the branch does (the result, the out `possessLoc`) and
// cancels; the native reads by a post-hook on GetPlayerCharacter (the broom's shape), armed while a record
// lives, whose source is the calling Kerfus. A record lives while its Kerfus is on, a haunting's possess
// drive included, and goes once it is off (pressed off, out of energy, the possess arrived) or its presser
// leaves. Nothing to replay at a join: the host's drive stream carries where it goes. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::kerfus_follow {

// On a host, register the targetActor watch and install the GetPlayerCharacter post-hook, disarmed
// (a client never records a presser). Idempotent; retried until both take, and a seam this build
// lacks is said once and not asked again. Game thread.
void Install(coop::net::Session* session);

// Drop the record of a Kerfus that is off or gone; the hook is armed while any record stays. Every
// pump tick; a loop over the Kerfuses a client turned on, none on a client. Game thread.
void Tick();

// HOST: a press of the Kerfus's on/off verb, by `slot` (0 = the host's own), at the verb's entry,
// before the body flips `active`: a press that turns it on names who it follows. Game thread.
void OnToggle(void* kerfus, uint8_t slot);

// A peer left: the Kerfuses it turned on follow the host again.
void OnPeerLeft(uint8_t slot);

// Session end: every record goes and the hook is disarmed.
void OnDisconnect();

}  // namespace coop::kerfus_follow
