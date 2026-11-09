// coop/dispatch/event_dispatch_dig.cpp -- a client's dig of a chip pile with a shovel or a rake: ScrapPileIntent
// (client to host, handed to coop::scrap_pile_intent); a shovel's hole in the ground: DugHoleState (host to client)
// and DugHoleIntent (client to host), both handed to coop::dug_hole_sync. Each case validates at the trust boundary. The switch below
// is the family's membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/props/scrap_pile_intent.h"
#include "coop/world/dug_hole_sync.h"

#include "ue_wrap/core/log.h"

#include <cmath>
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
    case net::ReliableKind::DugHoleState: {
        // HOST->CLIENT: a shovel hole the host stands, or its end. Host-authored: dropped on the host and from any
        // sender but the host. The id and the place are the module's to apply.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: DugHoleState from slot %d -- dropping (host-authored)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::DugHoleStatePayload)) {
            UE_LOGW("event_feed: DugHoleState payload too short (%zu < %zu)", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::DugHoleStatePayload));
            break;
        }
        net::DugHoleStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.id == 0 || p.alive > 1 || (p.alive && (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                                                      !std::isfinite(p.z)))) {
            UE_LOGW("event_feed: DugHoleState malformed -- dropping");
            break;
        }
        coop::dug_hole_sync::OnState(p);
        break;
    }
    case net::ReliableKind::DugHoleIntent: {
        // CLIENT->HOST: this client's shovel dug a hole, or its copy of a host hole ended by its own use. The host
        // checks the shovel, the reach and the rate in the module. Never relayed.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers ||
            msg.payloadLen < sizeof(net::DugHoleIntentPayload)) {
            UE_LOGW("event_feed: a malformed or misdirected DugHoleIntent from slot %d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        net::DugHoleIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::dug_hole_sync::OnIntent(session, msg.senderPeerSlot, p);
        break;
    }
    default:
        return false;  // not a dig kind -> event_feed tries the next family
    }
    return true;  // a dig kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
