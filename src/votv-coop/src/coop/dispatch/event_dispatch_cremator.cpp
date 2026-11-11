// coop/dispatch/event_dispatch_cremator.cpp -- the cremator: CrematorState (host to client, the machine's isActive,
// handed to the keyed channel), CrematorIntent (client to host, a handle press, handed to coop::cremator_intent) and
// CrematorHit (host to the victim, the machine's hit on that peer's player, handed to coop::cremator_hit). Each case
// validates at the trust boundary. The switch below is the family's membership declaration; nothing re-lists it.
// Family contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/cremator_intent.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/player/cremator_hit.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleCrematorEvent(net::Session& session,
                         const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::CrematorState: {
        // HOST->CLIENT: the cremator's isActive after its handle finished. Only the host authors it (a client's copy is
        // refused at the host's receive edge, client_authorship.h), so it is dropped on the host and from any sender
        // but slot 0. The keyed channel resolves the cremator by key and applies it, deferred when it has not streamed
        // in yet.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: CrematorState dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: CrematorState payload size %zu, not %zu", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: the action is a byte, but only 0 and 1 are meaningful, and the key is bounded by the wire.
        if (p.action != 0 && p.action != 1) {
            UE_LOGW("event_feed: CrematorState action=%u out of range -- dropping", static_cast<unsigned>(p.action));
            break;
        }
        if (p.key.len > sizeof(p.key.data)) {
            UE_LOGW("event_feed: CrematorState key length %u out of range -- dropping", static_cast<unsigned>(p.key.len));
            break;
        }
        coop::interactable_sync::OnReliable(static_cast<uint16_t>(msg.kind), p, 0);
        break;
    }
    case net::ReliableKind::CrematorIntent: {
        // CLIENT->HOST: a client pressed the cremator's handle, `action` the state it wants. The host judges the
        // sender's reach, its own handle and door, and that the state is not already so, and pulls its lever
        // (coop::cremator_intent). Never relayed.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CrematorIntent dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        net::KeyedTogglePayload p{};
        if (msg.payloadLen != sizeof(p)) {
            coop::cremator_intent::CountDropped(coop::cremator_intent::Drop::Size);
            break;
        }
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > sizeof(p.key.data)) {
            coop::cremator_intent::CountDropped(coop::cremator_intent::Drop::Key);
            break;
        }
        // The action is the state the press wants: only 0 and 1 are meaningful.
        if (p.action != 0 && p.action != 1) {
            coop::cremator_intent::CountDropped(coop::cremator_intent::Drop::Wanted);
            break;
        }
        coop::cremator_intent::OnIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::CrematorHit: {
        // HOST->OWNER: a cremator on the host hurt this peer's puppet. Only the host authors it (a client's copy is
        // refused at the host's receive edge, client_authorship.h), so it is dropped on the host and from any sender
        // but slot 0. The module resolves the cremator by key and applies the verb to this machine's own player.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: CrematorHit dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::CrematorHitPayload)) {
            UE_LOGW("event_feed: CrematorHit payload size %zu, not %zu", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::CrematorHitPayload));
            break;
        }
        net::CrematorHitPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: two verbs, and the key is bounded by the wire. The host never sends an empty key
        // (cremator_hit::Deliver refuses it), so one that arrives empty is not the host's.
        if (p.verb != net::kCrematorHitVerbDamage && p.verb != net::kCrematorHitVerbIgnite) {
            UE_LOGW("event_feed: CrematorHit verb=%u out of range -- dropping", static_cast<unsigned>(p.verb));
            break;
        }
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data)) {
            UE_LOGW("event_feed: CrematorHit key length %u out of range -- dropping", static_cast<unsigned>(p.key.len));
            break;
        }
        coop::cremator_hit::OnReceived(p);
        break;
    }
    default:
        return false;  // not a cremator kind -> event_feed tries the next family
    }
    return true;  // a cremator kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
