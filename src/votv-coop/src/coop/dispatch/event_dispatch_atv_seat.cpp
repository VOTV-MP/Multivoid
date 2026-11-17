// coop/dispatch/event_dispatch_atv_seat.cpp -- a client's seat in an ATV: AtvSeat (the driver's seat), AtvPassengerRequest
// (the passenger seats) and AtvTakeover (a grav-hand hold), each client to host and handed to the host's judge in
// coop::atv_authority. Each case validates at the trust boundary. The switch below is the family's membership
// declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is in
// this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_authority.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleAtvSeatEvent(net::Session& session,
                        const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::AtvSeat: {
        // CLIENT->HOST: this client's player sat in an ATV's driver seat or left it. The host judges the
        // request and answers it (coop::atv_authority::OnSeat). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: AtvSeat received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: AtvSeat from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvSeatPayload)) {
            UE_LOGW("event_feed: AtvSeat payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvSeatPayload));
            break;
        }
        net::AtvSeatPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::atv_authority::OnSeat(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::AtvPassengerRequest: {
        // CLIENT->HOST: this client's player boarded an ATV's passenger seats or left the one it holds. The host judges the
        // request and answers it (coop::atv_authority::OnPassengerRequest). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: AtvPassengerRequest received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: AtvPassengerRequest from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvPassengerRequestPayload)) {
            UE_LOGW("event_feed: AtvPassengerRequest payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvPassengerRequestPayload));
            break;
        }
        net::AtvPassengerRequestPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::atv_authority::OnPassengerRequest(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::AtvTakeover: {
        // CLIENT->HOST: this client's player took hold of an ATV with the grav hand. The host judges the
        // request and answers it (coop::atv_authority::OnTakeover). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: AtvTakeover received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: AtvTakeover from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvTakeoverPayload)) {
            UE_LOGW("event_feed: AtvTakeover payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvTakeoverPayload));
            break;
        }
        net::AtvTakeoverPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::atv_authority::OnTakeover(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    default:
        return false;  // not an ATV seat kind -> event_feed tries the next family
    }
    return true;  // an ATV seat kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
