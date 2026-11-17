// coop/cheats/cheat_request.h -- the cheat controls' one entry: a panel asks for a cheat and this module runs it where
// the authority is. On the host, or in a solo game, the cheat runs at once (coop/cheats/cheat_run). On a client it is
// sent to the host as a CheatRequest, which the host judges and runs or refuses, answering with CheatResult; the
// client never writes the world itself. A request is taken only while the host's master setting
// net.allow_player_cheats is on (Source's sv_cheats, reference/source-sdk-2013/src/game/server/client.cpp:1167) AND the
// host has not withheld the sender's right, the node multivoid.dev.local.cheats (MTA's per-right check,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CConsoleCommands.cpp:152). The host logs every request with the
// player's name and what came of it, and says in its chat what a player did.
//
// The panels call SpawnItem, AddPoints and FireEvent from the render thread; every engine touch is posted to the game
// thread. A request draws from a per-player bucket (coop/cheats/cheat_limits: ten tokens, three more a second; one
// token a request, one per prop for a spawn; a request the host refuses pays its first token too); one past it is
// dropped without work, answered Rate once and logged once per ten seconds. A client keeps a bucket of the same size,
// refilled a tenth slower, and offers no request it would not cover (CanAsk). A request from a sender whose world is
// not marked ready is dropped without work or answer.

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

// CLIENT: what this machine's panes know of its cheat rights, read once per frame by the menu: the host's master setting
// (`hostAllows`) and this machine's own right, the projected grant `Cheats` the host sends (`mine`, the node
// multivoid.dev.local.cheats). Offered only when both hold. A gate on the UI only: a client moves its own body without
// asking the host, and the host judges every request again. Any thread.
struct ClientRights {
    bool hostAllows = false;
    bool mine = false;
    bool Open() const { return hostAllows && mine; }
};
ClientRights ReadClientRights();

// Whether a panel may offer a request now: always on the host and in a solo game; on a client, once its world is
// announced to the host and while its mirror of the host's bucket holds what the request costs: one token, or `count`
// for a request for that many props. Any thread.
bool CanAsk(int count = 1);

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

// DRILL: charge `tokens` to this client's mirror of the host's bucket for a raw request the drill sent past the panel
// path, which draws nothing from it, so CanAsk keeps telling the truth about what the host will take. Nothing is spent
// when the mirror holds fewer. Game thread.
void ChargeMirrorForDrill(float tokens);

// One observer of every answer a client receives, for the dev drill; null clears. Game thread.
void SetResultObserver(void (*fn)(const coop::net::CheatResultPayload& p));

// HOST, per sender slot (1..): how many of its requests the host answered with `reason`, since the process began. The
// drill reads them against a baseline.
uint32_t AnsweredFor(int slot, coop::net::CheatReason reason);

// HOST: when the bucket of the sender in `slot` (1..) is full again, in coop::net::NowMs's clock; now when it is full
// already. The drill waits on it before an arm that needs the whole bucket. Game thread.
uint64_t RequestBucketFullAtMs(int slot);

// The session ended: the status and the observer start over.
void OnDisconnect();

}  // namespace coop::cheats
