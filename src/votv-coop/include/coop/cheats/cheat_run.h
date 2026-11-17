// coop/cheats/cheat_run.h -- the host's three cheats and the one place they are judged and run: spawn props, change
// the balance, fire a game event. The host's own panels and a player's request both end here, so a cheat means the
// same thing whoever asked, and the world is written by the host alone.
//
// A request from a player (`senderSlot` >= 1) arrives judged for its rights (coop/cheats/cheat_request: the host's
// setting net.allow_player_cheats and the sender's own right) and is judged here for the bounds on the numbers, the
// item against the game's own list and against the explosives a player may not spawn, the target against the
// session, and an event against OpenToPlayers (only a Safe event outside the story is a player's). The host's own act
// (slot 0) is judged only for shape. Nothing is sent here; the writes ride the existing lanes (the prop lane expresses
// a spawned prop, the balance poll mirrors a credit, the event watch broadcasts a fire). Game thread, on the host or in a
// solo game.

#pragma once

#include "coop/dev/event_trigger.h"
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

// Judge and run `r` for `senderSlot` (0 = the host's own act, 1.. = a player's request, whose rights are already
// judged); Refused for an event or a prop a player is not offered. Game thread.
Outcome Run(const Request& r, int senderSlot);

// True for an event a player may fire: its risk is Safe and it is not a story event. The one rule: the host's judge
// and a player's pane both ask it, of the table's row. Any thread.
bool OpenToPlayers(const coop::dev::event_trigger::EventInfo& ev);

// True for a prop a player may spawn: not one of the explosives. The one rule, like OpenToPlayers: the host's judge
// refuses what this denies and a player's Items pane does not list it. Any thread.
bool ItemOpenToPlayers(const std::string& name);

}  // namespace coop::cheats
