// coop/dispatch/event_dispatch_dig.cpp -- a client's dig of a chip pile with a shovel or a rake: ScrapPileIntent
// (client to host, handed to coop::scrap_pile_intent). The case validates at the trust boundary. The switch below
// is the family's membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/props/scrap_pile_intent.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleDigEvent(net::Session& session, const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::ScrapPileIntent: {
        // CLIENT->HOST dig REQUEST: the chip pile a client's shovel or rake dug. The host checks the hand and the
        // reach, then runs the pile's own dig itself, so the prop seams carry the item and the pile's death. The
        // tool, the reach and the rate live in the module; the format lives here. Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ScrapPileIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ScrapPileIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::ScrapPileIntentPayload)) {
            UE_LOGW("event_feed: ScrapPileIntent payload too short (%zu < %zu)", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::ScrapPileIntentPayload));
            break;
        }
        net::ScrapPileIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.eid == 0) {
            UE_LOGW("event_feed: ScrapPileIntent eid==0 -- dropping");
            break;
        }
        UE_LOGI("[SCRAP-PILE] RECEIVED eid=%u slot=%d", p.eid, msg.senderPeerSlot);
        coop::scrap_pile_intent::OnScrapPileIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    default:
        return false;  // not a dig kind -> event_feed tries the next family
    }
    return true;  // a dig kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
