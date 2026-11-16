// coop/cheats/cheat_request.h -- the cheat controls' one entry: a panel asks for a cheat and this module runs it where
// the authority is. On the host, or in a solo game, the cheat runs at once (coop/cheats/cheat_run). On a client it is
// sent to the host as a CheatRequest, which the host judges and runs or refuses, answering with CheatResult; the
// client never writes the world itself, and a request is only taken while the host's setting
// net.allow_player_cheats is on. MTA's shape: a client's command is a request the server checks and runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp, the command handler). The host logs every request a
// player makes, with the player's name and what came of it, and says in its chat what a player did.
//
// The panels call SpawnItem, AddPoints and FireEvent from the render thread; every engine touch is posted to the game
// thread. A request is bounded per player (a burst of eight, then three a second); one past its budget is dropped
// without work, answered Rate once and logged once per ten seconds.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::cheats {

// Cache the session; the host lane and the client route read it. Once, at boot.
void Install(coop::net::Session* session);

// The host's setting net.allow_player_cheats: on a client, the host's value as replicated. Any thread.
bool PlayersMayCheat();

// Ask for a cheat. Any thread. `target` is coop::net::kCheatInFront or the slot of the player to spawn at.
void SpawnItem(const std::string& name, int count, int target);
void AddPoints(int amount);
void FireEvent(const std::string& name);

// What came of the latest cheat this peer asked for, as a panel shows it. `valid` is false before the first.
struct Status {
    bool valid = false;
    coop::net::CheatOp op = coop::net::CheatOp::SpawnItem;
    coop::net::CheatReason reason = coop::net::CheatReason::Ok;
    int done = 0;
    bool pending = false;   // a client's request is on its way to the host
};
Status LastStatus();

// The status as one line in the player's language. Any thread.
std::string StatusLine(const Status& s);

// HOST: a client's request, judged and run or refused. Game thread. A request from an unknown sender is dropped.
void OnRequest(const coop::net::CheatRequestPayload& p, uint8_t senderSlot);

// CLIENT: the host's answer. Game thread.
void OnResult(const coop::net::CheatResultPayload& p);

// One observer of every answer a client receives, for the dev drill; null clears. Game thread.
void SetResultObserver(void (*fn)(const coop::net::CheatResultPayload& p));

// HOST, per sender slot (1..): how many of its requests the host answered with `reason`, since the process began. The
// drill reads them against a baseline.
uint32_t AnsweredFor(int slot, coop::net::CheatReason reason);

// The session ended: the status and the observer start over.
void OnDisconnect();

}  // namespace coop::cheats
