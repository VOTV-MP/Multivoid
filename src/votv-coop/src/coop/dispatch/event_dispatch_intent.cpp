// coop/event_dispatch_intent.cpp -- the CLIENT->HOST INTENT / REQUEST
// reliable-kind case bodies: a client asks the HOST to perform an action it
// is authoritative for (OrderRequest, DoorOpenRequest, KerfurConvertRequest,
// KerfurCommand, GrabIntent, ThrowIntent, PileResyncRequest, PropDropIntent).
//
// EXTRACTED VERBATIM from event_dispatch_state.cpp 2026-07-10 (816 LOC, past
// the 800 soft cap; the queued event_dispatch_intent extraction). The intent
// family is a distinct concept from keyed device-STATE mirrors: every case
// here validates role()==Host + a client sender slot at the trust boundary,
// then hands the request to the authoritative module. Family contract per
// coop/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_convert.h"
#include "coop/creatures/roach_sync.h"    // v108: CLIENT->HOST local roach consumption intent
#include "coop/interactables/interactable_sync.h"
#include "coop/items/order_sync.h"
#include "coop/props/prop_drop_intent.h"  // v106 F2 Inc-1: CLIENT->HOST client-placed keyed prop
#include "coop/props/trash_channel.h"

#include "ue_wrap/log.h"
#include "ue_wrap/types.h"  // ue_wrap::FVector (ThrowIntent camFwd)

#include <cmath>
#include <cstring>

namespace coop::event_feed {

bool HandleIntentEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg,
                       void* localPlayer) {
    (void)localPlayer;  // symmetric family signature; current cases resolve targets by key/eid
    switch (msg.kind) {
    case net::ReliableKind::OrderRequest: {
        // v49 (2026-06-09): delivery-drone ECONOMY -- a CLIENT forwards a laptop shop order to the
        // HOST (the delivery authority). VARIABLE-LENGTH (OrderRequestHeader + packed items); the
        // host assembles chunks per (senderSlot, orderId) then re-commits via the native
        // makeAnOrder. order_sync::OnReliable fully range-checks the payload + no-ops on a client
        // (host-only ingest). RE: votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.
        // OrderRequest is CLIENT->HOST: a valid sender is a CLIENT slot (1..kMaxPeers-1). Slot 0
        // is the host (which never sends its own order as a request -- it is the delivery
        // authority); an out-of-range / not-yet-assigned slot must NOT be routed under a 0xFF
        // sentinel that would create host assembly state in a shared bucket. Drop at the boundary.
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: OrderRequest from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        coop::order_sync::OnReliable(msg.payload, static_cast<int>(msg.payloadLen),
                                     static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::DoorOpenRequest: {
        // v32 (2026-06-04): client->host door open/close REQUEST. Doors are HOST-
        // authoritative; only the host honors this. The host applies it (real lock/
        // jam guards) and its poll broadcasts the authoritative DoorState back to all.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DoorOpenRequest received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: DoorOpenRequest payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.action != 0 && p.action != 1) {
            UE_LOGW("event_feed: DoorOpenRequest action=%u out of range -- dropping",
                    static_cast<unsigned>(p.action));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::interactable_sync::OnDoorOpenRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurConvertRequest: {
        // v67: client->host kerfur on/off conversion request. Host-authoritative
        // (the DoorOpenRequest shape); the handler validates the element + class
        // + the BP kill-guard, runs the real verb, and converges the BP-internal
        // spawn/destroy side effects onto the wire (coop::kerfur_convert).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfurConvertRequest received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfurConvertPayload)) {
            UE_LOGW("event_feed: KerfurConvertRequest payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurConvertPayload));
            break;
        }
        net::KerfurConvertPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.toProp != 0 && p.toProp != 1) {
            UE_LOGW("event_feed: KerfurConvertRequest toProp=%u out of range -- dropping",
                    static_cast<unsigned>(p.toProp));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::kerfur_convert::OnConvertRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurCommand: {
        // v74: client->host kerfur radial-menu command (follow/idle/patrol/...). Host-authoritative;
        // the handler validates the element + kerfur class + the BP kill-guard, then runs the verb
        // (or, for Follow, starts the host-side MoveTo loop toward the REQUESTING player's body --
        // senderSlot). coop::kerfur_command.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfurCommand received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfurCommandPayload)) {
            UE_LOGW("event_feed: KerfurCommand payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurCommandPayload));
            break;
        }
        net::KerfurCommandPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::kerfur_command::OnCommandRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::GrabIntent: {
        // v84 (Increment 2, docs/piles/08): CLIENT->HOST chipPile grab REQUEST. Host-authoritative
        // (the DoorOpenRequest shape). The host validates the eid is a tracked PILED pile + the sender
        // isn't already holding one, executes playerGrabbed on puppet-N (probe-proven, see
        // research/findings/votv-puppet-grab-feasibility-RE-2026-06-22.md), and broadcasts the
        // authoritative PropConvert{kToClump}. coop::trash_channel::OnGrabIntent.
        // NOTE (modular, RULE 2026-05-25): this file is ~757 LOC after this case (past the 800 soft cap
        // is near) -- the NEXT trash-intent handler (ThrowIntent/PileResyncRequest, phase 2) MUST extract
        // the trash-intent cases into a new event_dispatch_trash.cpp.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: GrabIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: GrabIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::GrabIntentPayload)) {
            UE_LOGW("event_feed: GrabIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::GrabIntentPayload));
            break;
        }
        net::GrabIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.eid == 0) {
            UE_LOGW("event_feed: GrabIntent eid==0 -- dropping");
            break;
        }
        UE_LOGI("[GRAB-INTENT] RECEIVED eid=%u slot=%d", p.eid, msg.senderPeerSlot);
        coop::trash_channel::OnGrabIntent(session, p.eid, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::ThrowIntent: {       // v85 (Increment 2 phase 2): CLIENT->HOST throw of a puppet-held clump
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ThrowIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ThrowIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::ThrowIntentPayload)) {
            UE_LOGW("event_feed: ThrowIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ThrowIntentPayload));
            break;
        }
        net::ThrowIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.eid == 0) {
            UE_LOGW("event_feed: ThrowIntent eid==0 -- dropping");
            break;
        }
        UE_LOGI("[THROW-INTENT] RECEIVED eid=%u slot=%d mode=%u", p.eid, msg.senderPeerSlot, p.mode);
        coop::trash_channel::OnThrowIntent(session, p.eid, p.mode,
                                           ue_wrap::FVector{p.dirX, p.dirY, p.dirZ},
                                           static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::PileResyncRequest:  // v84 STAGED (Increment 2 phase 3) -- ID reserved, no handler yet
        UE_LOGI("event_feed: STAGED ReliableKind=%u from slot=%d -- no handler yet",
                static_cast<unsigned>(msg.kind), msg.senderPeerSlot);
        break;
    case net::ReliableKind::PropDropIntent: {   // v106 (F2 Inc-1): CLIENT->HOST -- client placed a keyed
                                                // world prop it had picked up; the HOST re-spawns it
                                                // authoritatively by Key + broadcasts. Host-authoritative
                                                // (the GrabIntent shape). coop::prop_drop_intent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PropDropIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PropDropIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PropDropIntentPayload)) {
            UE_LOGW("event_feed: PropDropIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropDropIntentPayload));
            break;
        }
        net::PropDropIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::prop_drop_intent::OnPropDropIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::RoachConsumed: {  // v108: CLIENT->HOST -- a native eat/stomp destroyed a
                                              // roach component locally; the host deletes its nearest
                                              // roach and the next RoachState converges every peer.
                                              // Role/sender validation in roach_sync::OnConsumedIntent.
        if (msg.payloadLen < sizeof(net::RoachConsumedPayload)) {
            UE_LOGW("event_feed: RoachConsumed payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::RoachConsumedPayload));
            break;
        }
        net::RoachConsumedPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::roach_sync::OnConsumedIntent(p, msg.senderPeerSlot);
        break;
    }
    default:
        return false;  // not an intent-family kind -> event_feed tries the next family
    }
    return true;  // an intent-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
