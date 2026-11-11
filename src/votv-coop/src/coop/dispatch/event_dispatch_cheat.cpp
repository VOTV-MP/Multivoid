// coop/dispatch/event_dispatch_cheat.cpp -- a player's cheat request and the host's answer: CheatRequest (client to
// host, handed to the host's judge in coop::cheats) and CheatResult (host to client, shown by the same module). Each
// case validates at the trust boundary. The switch below is the family's membership declaration; nothing re-lists it.
// Family contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/cheats/cheat_request.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleCheatEvent(net::Session& session, const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::CheatRequest: {
        // CLIENT->HOST: a player asks for a cheat. The host judges the setting, the sender and the request itself, runs
        // it or refuses it, and answers with CheatResult. Never relayed. coop::cheats::OnRequest.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CheatRequest dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::CheatRequestPayload)) {
            UE_LOGW("event_feed: CheatRequest from slot %d dropped (length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::CheatRequestPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::cheats::OnRequest(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::CheatResult: {
        // HOST->CLIENT: what the host did with this client's CheatRequest. Never relayed. coop::cheats::OnResult.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen != sizeof(net::CheatResultPayload)) {
            UE_LOGW("event_feed: CheatResult dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::CheatResultPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::cheats::OnResult(p);
        break;
    }
    default:
        return false;  // not a cheat kind -> event_feed tries the next family
    }
    return true;  // a cheat kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
