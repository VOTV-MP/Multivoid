// coop/props/chip_scrap_sync.h -- a shovel's or a rake's scoop of a trash pile is the host's.
//
// The shovel's dig (mainPlayer_C's montage notify) and the rake run the pile's own actorChipPile_C::turnIntoScrap, which
// mints the pile's scrap (a prop) and destroys the pile. A client ran it on its own copy alone: it saw the scrap and
// the pile gone, and every other player kept the pile. CLIENT: its turnIntoScrap is cancelled and asked of the host
// (ChipScrapAsk: the pile's element id, and its place for a pile with none). HOST: it judges the sender's reach and runs
// the pile's turnIntoScrap on its own copy; the scrap and the pile's end cross as props. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ChipScrapAskPayload;
}  // namespace coop::net

namespace coop::chip_scrap_sync {

void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once a tick.
void Tick();

// HOST: one client's ask, from the dispatch.
void OnAsk(uint8_t senderSlot, const coop::net::ChipScrapAskPayload& p);

void OnDisconnect();

}  // namespace coop::chip_scrap_sync
