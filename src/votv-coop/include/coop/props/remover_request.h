// coop/props/remover_request.h -- the tool gun's remover as a request the host performs.
//
// tool_remover_C::init(toolgun) destroys the actor the toolgun's trace struck, whoever pressed; a client's destroy
// used to reach the host through the prop seam and be applied there type-blind. A client now cancels the body,
// destroys its tool, and sends RemoverRequest: the element it hit, the camera point it looked from and the point its
// shot struck. A hit it cannot read or place fails closed ("Not removable right now"); an actor the session does not
// share runs natively; the host and a solo game run the game's body.
//
// The host queues a client's requests (cap 8, a pace of 4 then 2 a second), serves one head a tick and answers each
// with RemoverResult. The right is the node multivoid.gameplay.toolgun.remove, default granted. A client shows NoRight,
// Unsupported, Held, OutOfSight and Failed once per reason per five seconds. A modified client that skips the cancel still
// destroys its own copy, whose PropDestroy the host relays type-blind (session_lanes.h): that half waits for the
// destroy judge. MTA's shape: a client never destroys a server-owned element
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp) and the server checks the right
// where the request arrives (reference/mtasa-blue/Server/mods/deathmatch/logic/CRegisteredCommands.cpp).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct RemoverRequestPayload;
struct RemoverResultPayload;
enum class RemoverReason : uint8_t;
}  // namespace coop::net

namespace coop::remover_request {

// Cache the session and take the init watch (once per process). Both roles.
void Install(coop::net::Session* session);

// Settle the watch's registration with one line; on a running host, serve each sender's queue head and flush the
// refusal tally. Game thread.
void Tick();

// HOST: a client's request, queued. An unknown sender or a full queue is refused into the tally. Game thread.
void OnRequest(const coop::net::RemoverRequestPayload& payload, uint8_t senderSlot);

// HOST: a refusal the router made (a short payload), into the lane's tally.
void NoteRefused(uint8_t senderSlot, const char* why);

// CLIENT: the host's answer; the five visible reasons are shown. Game thread.
void OnResult(const coop::net::RemoverResultPayload& payload);

// The tally emptied and the shown notices forgotten. A sender's queue and counts go with its occupant (the roster
// ledger clears them), not here. Net disconnect.
void OnDisconnect();

// HOST, per sender (slots 1 and up; 0 for anything else): the requests the lane took off a queue to judge, those it
// refused at the push (a full queue, an unknown sender, a short payload), and the answers it gave per reason. The
// hostile drill reads them against a baseline.
uint32_t RunsFor(int slot);
uint32_t RefusedFor(int slot);
uint32_t AnsweredFor(int slot, coop::net::RemoverReason reason);
// The branch of the last refusal answered to `slot` (a static string), or null: what the hostile drill reads to tell
// which check stopped a request, not only that one did.
const char* LastWhyFor(int slot);
// The class that stopped the last blocked sight segment (empty before any): a wall is not a player body.
const wchar_t* LastBlockerClass();

}  // namespace coop::remover_request
