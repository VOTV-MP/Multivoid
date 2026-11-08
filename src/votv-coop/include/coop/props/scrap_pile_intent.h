// coop/props/scrap_pile_intent.h -- digging a chip pile with a shovel or a rake, as a client's intent the host
// performs. Gameplay/network layer (principle 7); the engine is reached through ue_wrap::dig_tool and ue_wrap::prop.
//
// The dig ends in the pile's own turnIntoScrap, which spawns one trash item named by the pile's
// chipType, destroys the pile and plays the dig sound in 2D (ue_wrap/actors/dig_tool.h). The host's own
// dig needs nothing here: its item's birth and its pile's death ride the prop seams to every peer. On a
// client it authored nothing the host sees -- a client's prop birth is refused at the door -- so the
// pile stayed on the host and the item stayed on the client.
//
// This lane refuses the pile's body on a client at the script-body gate and sends the pile's element id
// instead, the way a bag is packed (coop/props/pack_trash_intent). The host checks that the sender holds a
// shovel or a rake on its mirror of that hand and stands within reach of the pile, then runs the pile's own
// turnIntoScrap inside the sender's feedback scope (coop/requester_feedback): the item and the pile's
// death leave the host on the prop seams, and the dig sound plays for the digger instead of the host.
// Late join: nothing to replay, since a dig the host has not run changed nothing.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ScrapPileIntentPayload;
}  // namespace coop::net

namespace coop::scrap_pile_intent {

// Watch the pile's turnIntoScrap (client) and cache the session. Idempotent; retried until the watch's
// name resolves. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: keep the watch pumped, and run one queued dig per peer per token (host). Game thread.
void Tick(coop::net::Session& session);

// HOST: a client asks for a dig. Queued here and performed from Tick under the rate limit.
void OnScrapPileIntent(coop::net::Session& session, const coop::net::ScrapPileIntentPayload& payload,
                       uint8_t senderSlot);

// Session teardown: the counts are said. A sender's queue lives in its per-slot state and goes with its
// occupant, so the next occupant of the slot starts clean.
void OnDisconnect();

}  // namespace coop::scrap_pile_intent
