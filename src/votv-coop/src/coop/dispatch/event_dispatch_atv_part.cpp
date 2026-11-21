// coop/dispatch/event_dispatch_atv_part.cpp -- a client's work on an ATV's wheels or spare: AtvPartIntent
// (client to host, handed to the host's judge in coop::atv_part_intent), and a client's spawn-menu ATV: AtvSpawnAsk
// (coop::atv_spawn_ask). The case validates at the trust boundary.
// The switch below is the family's membership declaration; nothing re-lists it. Family contract per
// coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_part_intent.h"
#include "coop/interactables/atv_spawn_ask.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleAtvPartEvent(net::Session& session, const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::AtvPartIntent: {
        // CLIENT->HOST: a client unbolted, fitted, stowed or took a part of an ATV; the host runs the change on its
        // copy. Never relayed. coop::atv_part_intent::OnIntent judges the sender and the ATV.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: AtvPartIntent dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::AtvPartIntentPayload)) {
            UE_LOGW("event_feed: AtvPartIntent from slot %d dropped (length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::AtvPartIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::atv_part_intent::OnIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::AtvSpawnAsk: {
        // CLIENT->HOST: a client's spawn-menu ATV, asked of the host (coop::atv_spawn_ask judges it). Never relayed.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers ||
            msg.payloadLen != sizeof(net::AtvSpawnAskPayload)) {
            UE_LOGW("event_feed: AtvSpawnAsk dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::AtvSpawnAskPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::atv_spawn_ask::OnAsk(session, p, msg.senderPeerSlot);
        break;
    }
    default:
        return false;  // not an ATV part kind -> event_feed tries the next family
    }
    return true;  // an ATV part kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
