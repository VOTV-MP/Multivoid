// coop/cheats/cheat_run.h -- the host's three cheats and the one place they are judged and run: spawn props, change
// the balance, fire a game event. The host's own panels and a player's request both end here, so a cheat means the
// same thing whoever asked, and the world is written by the host alone.
//
// A request from a player (`senderSlot` >= 1) is judged first: the host's setting net.allow_player_cheats, the bounds
// on the numbers, the item against the game's own list, the target against the session, and an event against its
// risk -- an event that moves players or advances the story is the host's alone. The host's own act (slot 0) is
// judged only for shape. Nothing is sent here; the writes ride the existing lanes (the prop lane expresses a spawned
// prop, the balance poll mirrors a credit, the event watch broadcasts a fire). Game thread, on the host or in a solo
// game.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::cheats {

// One cheat as asked, from the host's panel or a player's wire request.
struct Request {
    coop::net::CheatOp op = coop::net::CheatOp::SpawnItem;
    std::string name;      // SpawnItem: the prop; Event: the event
    int count = 1;         // SpawnItem
    int target = coop::net::kCheatInFront;   // SpawnItem: kCheatInFront or a slot
    int amount = 0;        // Points, signed
};

// What came of it; `done` is how many props were spawned (SpawnItem), else 0.
struct Outcome {
    coop::net::CheatReason reason = coop::net::CheatReason::Failed;
    int done = 0;
};

// Judge and run `r` for `senderSlot` (0 = the host's own act, 1.. = a player's request). A player's request is refused
// with Off while the host's setting is off. Game thread.
Outcome Run(const Request& r, int senderSlot);

// True for an event a player may ask for: not the story, save or relocation events (Risk::Dangerous). The player panel
// lists only these. Any thread.
bool EventOpenToPlayers(const char* eventName);

}  // namespace coop::cheats
