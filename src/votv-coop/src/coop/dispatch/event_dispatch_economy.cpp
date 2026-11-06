// coop/dispatch/event_dispatch_economy.cpp -- the ECONOMY reliable-kind case bodies: the delivery order
// and its queue, the coin gun's sale and collect, the laptop upgrade purchase and the point sack. The
// switch below is the family's membership declaration; nothing re-lists it.
//
// Direction differs by case, so read the case: OrderRequest, CoinGunSell and CoinCollect check the
// client sender slot here and leave the role gate to the module they call, which drops off the host;
// UpgradeIntent and PointSackRedeem check role()==Host AND a client sender slot; CoinGunResult and
// OrderRefused are HOST->CLIENT answers and OrderQueue is the host's delivery queue mirrored to
// clients, all three refused from a client at the host's receive edge (client_authorship.h). Family
// contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/upgrade_sync.h"  // CLIENT->HOST upgrade purchase
#include "coop/items/coingun_sync.h"
#include "coop/items/order_queue_sync.h"  // HOST->CLIENT the delivery order queue
#include "coop/items/order_sync.h"
#include "coop/items/point_sack_intent.h"  // CLIENT->HOST an opened point sack

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleEconomyEvent(net::Session& session,
                        const net::Session::ReliableMessage& msg,
                        void* localPlayer) {
    // CoinCollect is the family's first consumer of `localPlayer` -- the host passes its own
    // mainPlayer into the coin's `actionOptionIndex`. Every other case resolves targets by key/eid.
    switch (msg.kind) {
    case net::ReliableKind::OrderRequest: {
        // Delivery-drone ECONOMY: a CLIENT forwards a laptop shop order to the HOST (the
        // delivery authority). VARIABLE-LENGTH (OrderRequestHeader + packed items); the host
        // assembles chunks per (senderSlot, orderId) then re-commits via the native makeAnOrder.
        // order_sync::OnReliable fully range-checks the payload and no-ops on a client
        // (host-only ingest).
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
    case net::ReliableKind::CoinGunSell: {
        // A CLIENT shot a prop
        // with the coin gun. The payload NAMES the prop (save key first, ElementId as the keyless
        // fallback) and nothing else; the host prices the sale from its OWN copy and mints through
        // the game's own `sell`. HOST-TERMINAL -- never relayed, because the host authors every
        // consequence itself (coins via WorldActorSpawn, the prop's removal via the client's own
        // unchanged PropDestroy, which arrives right behind this on the same lane).
        // CLIENT->HOST: slot 0 is the host, which never sends itself a sale. Drop at the boundary
        // rather than routing an unassigned slot under a sentinel.
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CoinGunSell from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        coop::coingun_sync::OnReliable(msg.payload, static_cast<int>(msg.payloadLen),
                                       static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::CoinCollect: {
        // coingun_sync: a CLIENT collected a coin that mirrors one of the host's, and forwards it
        // because it cannot perform the collect itself (`lib_C::addPoints` is
        // EX_LocalVirtualFunction -- no peer but the owner may author a credit). The host runs the
        // coin's own `actionOptionIndex`, so the native credit and self-destroy are the game's.
        // HOST-TERMINAL -- never relayed; the consequence reaches the other peers as the coin's
        // ordinary WorldActorDestroy. CLIENT->HOST: slot 0 is the host, which never forwards its
        // own collect.
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CoinCollect from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        coop::coingun_sync::OnCoinCollect(msg.payload, static_cast<int>(msg.payloadLen),
                                          static_cast<uint8_t>(msg.senderPeerSlot), localPlayer);
        break;
    }
    case net::ReliableKind::CoinGunResult: {
        // coingun_sync: the HOST's answer to ONE client's sale. HOST->CLIENT, addressed with
        // SendReliableToSlot, never relayed. The seller's prop is already gone from its own screen
        // (its destroy is deliberately unchanged and still lands), so a refusal that says NOTHING
        // renders as a silently vanished item -- the sentence is the point. A success carries the
        // price the HOST used, which can legitimately differ from the seller's own local toast
        // (`getPriceMultiplier` is per-instance and divergent).
        coop::coingun_sync::OnReliableResult(msg.payload, static_cast<int>(msg.payloadLen));
        break;
    }
    case net::ReliableKind::OrderQueue: {
        // The host's delivery order queue, as each change ran (reset, append, pop); a client mirrors
        // it and never writes its own. HOST->CLIENT, never relayed; a client's copy is refused at the
        // host's receive edge (client_authorship.h).
        coop::order_queue_sync::OnReliable(msg.payload, static_cast<int>(msg.payloadLen));
        break;
    }
    case net::ReliableKind::OrderRefused: {
        // The HOST tells ONE client that its forwarded shop order
        // was not performed, and why. HOST->CLIENT, addressed with SendReliableToSlot, never
        // relayed. A client that receives this has ALREADY debited itself locally (Button_order
        // @6168 runs lib_C::addPoints before we ever see the order, and that call is
        // EX_LocalVirtualFunction, so it cannot be suppressed) -- the correction rides a direct
        // BalanceSync the host sends alongside; this message carries the REASON and the orderId the
        // client needs to rebuild its cart.
        coop::order_sync::OnReliableRefused(msg.payload, static_cast<int>(msg.payloadLen));
        break;
    }
    case net::ReliableKind::UpgradeIntent: {
        // CLIENT->HOST upgrade purchase REQUEST: a client pressed the laptop panel's buy or sell
        // button. Its own body was cancelled at the script gate, so it has not paid and has not
        // raised its level. A level row (dir 0 and 1): the host re-derives the price and the bounds
        // from its own table and charges itself. A module row (dir 2 and 3): the host presses its
        // own row of that index and the game's body decides. The levels are then republished. The
        // row carries no identity but its index, so holding the laptop claim is the sender's reach
        // -- that test, the arithmetic and the rate live in the module; the format lives here.
        // coop::upgrade_sync::OnUpgradeIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: UpgradeIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: UpgradeIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::UpgradeIntentPayload)) {
            UE_LOGW("event_feed: UpgradeIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::UpgradeIntentPayload));
            break;
        }
        net::UpgradeIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.dir > net::upgrade_intent::kModuleSell) {
            UE_LOGW("event_feed: UpgradeIntent dir=%u has no lane -- dropping",
                    static_cast<unsigned>(p.dir));
            break;
        }
        coop::upgrade_sync::OnUpgradeIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::PointSackRedeem: {
        // CLIENT->HOST: a client opened a point sack; the host pays it from its copy and destroys it. Never relayed.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers ||
            msg.payloadLen < sizeof(net::PointSackRedeemPayload)) {
            UE_LOGW("event_feed: PointSackRedeem dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::PointSackRedeemPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::point_sack_intent::OnRedeem(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    default:
        return false;  // not an economy-family kind -> event_feed tries the next family
    }
    return true;  // an economy-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
