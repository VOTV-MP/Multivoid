// coop/dispatch/event_dispatch_cargo_lift.cpp -- the cargo lift: CargoLiftState (host to client, the lift's direction,
// handed to the keyed channel) and CargoLiftIntent (client to host, a call-button press, handed to coop::cargo_lift).
// Each case validates at the trust boundary. The switch below is the family's membership declaration; nothing re-lists
// it. Family contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/cargo_lift.h"
#include "coop/interactables/interactable_sync.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleCargoLiftEvent(net::Session& session,
                          const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::CargoLiftState: {
        // HOST->CLIENT: the lift's direction after a press or after its own obstruction reversal. Only the host
        // authors it (a client's copy is refused at the host's receive edge, client_authorship.h), so it is dropped on
        // the host and from any sender but slot 0. The keyed channel resolves the lift by key and applies it, deferred
        // when it has not streamed in yet.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: CargoLiftState dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: CargoLiftState payload size %zu, not %zu", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: a key the wire can hold, and a direction that is a bool.
        if (p.key.len > sizeof(p.key.data) || p.action > 1) {
            UE_LOGW("event_feed: CargoLiftState key length %u or action %u out of range -- dropping",
                    static_cast<unsigned>(p.key.len), static_cast<unsigned>(p.action));
            break;
        }
        coop::interactable_sync::OnReliable(static_cast<uint16_t>(msg.kind), p, 0);
        break;
    }
    case net::ReliableKind::CargoLiftIntent: {
        // CLIENT->HOST: a client pressed the cargo lift's call button, `action` the direction it wants. The lift's
        // resolve, the reach and the rate live in the module; the format lives here. coop::cargo_lift::OnIntent.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CargoLiftIntent dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        net::KeyedTogglePayload p{};
        if (msg.payloadLen != sizeof(p)) {
            UE_LOGW("event_feed: CargoLiftIntent payload size %zu, not %zu", static_cast<size_t>(msg.payloadLen),
                    sizeof(p));
            break;
        }
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > sizeof(p.key.data) || p.action > 1) {
            UE_LOGW("event_feed: CargoLiftIntent key length %u or action %u out of range -- dropping",
                    static_cast<unsigned>(p.key.len), static_cast<unsigned>(p.action));
            break;
        }
        coop::cargo_lift::OnIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    default:
        return false;
    }
    return true;
}

}  // namespace coop::event_feed
