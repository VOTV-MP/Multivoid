// coop/dispatch/event_dispatch_intent.cpp -- the CLIENT->HOST INTENT / REQUEST reliable-kind case
// bodies: a client asks the HOST to perform an action it alone is authoritative for. The switch
// below is the family's membership declaration; nothing re-lists it.
//
// The intent family is a distinct concept from keyed device-STATE mirrors. MOST cases are
// CLIENT->HOST and gate on role()==Host plus a client sender slot (1..kMaxPeers-1) at the
// trust boundary before handing the request to the authoritative module -- but not all of
// them, so read the case: CoinGunResult and OrderRefused are HOST->CLIENT answers and drop
// on the host instead, and OrderQueue, the host's delivery queue mirrored to clients, leaves
// both checks to order_queue_sync; OrderRequest, CoinGunSell and CoinCollect check the sender slot here
// and leave the role gate to the module they call, which drops off the host; and RoachConsumed
// defers both to roach_sync::OnConsumedIntent. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_convert_host.h"
#include "coop/creatures/roach_sync.h"    // CLIENT->HOST local roach consumption intent
#include "coop/interactables/door_verb_intent.h"  // CLIENT->HOST press, hit or pry of a base door
#include "coop/interactables/keypad_verbs.h"  // CLIENT->HOST digit, submit, cancel, keycard, reset
#include "coop/interactables/drone_call_intent.h"  // CLIENT->HOST press of the drone console
#include "coop/creatures/kerfus_intent.h"  // CLIENT->HOST a Kerfus verb (on/off, fix the servers, pat)
#include "coop/items/coingun_sync.h"
#include "coop/items/order_queue_sync.h"  // HOST->CLIENT the delivery order queue
#include "coop/items/order_sync.h"
#include "coop/props/pack_trash_intent.h"  // CLIENT->HOST bagging of a pile or clump
#include "coop/props/prop_drop_intent.h"  // CLIENT->HOST client-placed keyed prop
#include "coop/items/broom_stroke.h"
#include "coop/interactables/upgrade_sync.h"  // CLIENT->HOST upgrade purchase
#include "coop/props/trash_channel.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"  // ue_wrap::FVector (ThrowIntent camFwd)

#include <cmath>
#include <cstring>
#include <string>

namespace coop::event_feed {

bool HandleIntentEvent(net::Session& session,
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
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: CoinGunResult received on the HOST -- dropping");
            break;
        }
        coop::coingun_sync::OnReliableResult(msg.payload, static_cast<int>(msg.payloadLen));
        break;
    }
    case net::ReliableKind::OrderQueue: {
        // The host's delivery order queue, as each change ran (reset, append, pop); a client mirrors
        // it and never writes its own. HOST->CLIENT, never relayed; order_queue_sync checks the
        // sender is slot 0 and no-ops on the host.
        coop::order_queue_sync::OnReliable(msg.payload, static_cast<int>(msg.payloadLen),
                                           static_cast<uint8_t>(msg.senderPeerSlot));
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
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: OrderRefused received on the HOST -- dropping");
            break;
        }
        coop::order_sync::OnReliableRefused(msg.payload, static_cast<int>(msg.payloadLen));
        break;
    }
    case net::ReliableKind::DoorVerbIntent: {
        // CLIENT->HOST: a client pressed, hit or pried a base door, and the host runs the same entry
        // verb on its own copy. The door's resolve, the reach and the rate live in the module; the
        // format lives here. coop::door_verb_intent::OnDoorVerbIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DoorVerbIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: DoorVerbIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::DoorVerbIntentPayload)) {
            UE_LOGW("event_feed: DoorVerbIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DoorVerbIntentPayload));
            break;
        }
        net::DoorVerbIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: one of the three verbs, and a damage a door's body can take -- finite
        // and not negative, since it becomes the panels' interpolation speed.
        if (p.verb > net::door_verb::kPry) {
            UE_LOGW("event_feed: DoorVerbIntent verb=%u out of range -- dropping",
                    static_cast<unsigned>(p.verb));
            break;
        }
        if (!std::isfinite(p.damage) || p.damage < 0.f) {
            UE_LOGW("event_feed: DoorVerbIntent damage=%f unusable -- dropping",
                    static_cast<double>(p.damage));
            break;
        }
        coop::door_verb_intent::OnDoorVerbIntent(session, p,
                                                 static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::KeypadIntent: {
        // CLIENT->HOST: a client typed, submitted, cancelled, swiped a keycard or used a pass changer
        // on a keypad, and the host runs the verb on its own copy. The keypad's resolve, the reach,
        // the held item and the rate live in the module; the format lives here.
        // coop::keypad_verbs::OnKeypadIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KeypadIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: KeypadIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::KeypadIntentPayload)) {
            UE_LOGW("event_feed: KeypadIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeypadIntentPayload));
            break;
        }
        net::KeypadIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: a known verb, and a digit that is one.
        if (p.verb > net::keypad_intent::kMax ||
            (p.verb == net::keypad_intent::kDigit && p.arg > 9)) {
            UE_LOGW("event_feed: KeypadIntent verb=%u arg=%u out of range -- dropping",
                    static_cast<unsigned>(p.verb), static_cast<unsigned>(p.arg));
            break;
        }
        coop::keypad_verbs::OnKeypadIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::KerfurConvertRequest: {
        // client->host kerfur on/off conversion request. Host-authoritative (the client asks and
        // the host runs the verb); the handler validates the element + class + the BP kill-guard, runs the real
        // verb, and converges the BP-internal spawn/destroy side effects onto the wire
        // (coop::kerfur_convert).
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
        coop::kerfur_convert_host::OnConvertRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurCommand: {
        // client->host kerfur radial-menu command (follow/idle/patrol/...). Host-authoritative; the
        // handler validates the element + kerfur class + the BP kill-guard, then runs the verb (or,
        // for Follow, starts the host-side MoveTo loop toward the REQUESTING player's body --
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
    case net::ReliableKind::GrabRefused: {
        // HOST->CLIENT, one recipient, never relayed: the answer to a GrabIntent the host did not
        // perform, or (reason HoldEnded) the notice that the host ended the recipient's carry.
        // coop::trash_channel::OnGrabRefused.
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: GrabRefused received on the HOST -- dropping");
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: GrabRefused from senderPeerSlot=%d, not the host -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::GrabRefusedPayload)) {
            UE_LOGW("event_feed: GrabRefused payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::GrabRefusedPayload));
            break;
        }
        net::GrabRefusedPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::trash_channel::OnGrabRefused(p.eid, p.reason, p.reqId);
        break;
    }
    case net::ReliableKind::UpgradeIntent: {
        // CLIENT->HOST upgrade purchase REQUEST: a client pressed the laptop panel's buy or sell
        // button. Its own body was cancelled at the script gate, so it has not paid and has not
        // raised its level; the host re-derives the price and the bounds from its own table and
        // charges itself, then republishes the levels. The row carries no identity but its index,
        // so holding the laptop claim is the sender's reach -- that test, the arithmetic and the
        // rate live in the module; the format lives here.
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
        if (p.dir > 1) {
            UE_LOGW("event_feed: UpgradeIntent dir=%u has no lane -- dropping",
                    static_cast<unsigned>(p.dir));
            break;
        }
        coop::upgrade_sync::OnUpgradeIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::GrabIntent: {
        // CLIENT->HOST chipPile grab REQUEST. Host-authoritative (the client asks and the host runs
        // the verb).
        // The host validates the eid is a tracked PILED pile and that the sender is not already
        // holding one, executes playerGrabbed on puppet-N, and broadcasts the authoritative
        // PropConvert{kToClump}. coop::trash_channel::OnGrabIntent.
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
        coop::trash_channel::OnGrabIntent(session, p.eid, p.reqId, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::PackTrashIntent: {
        // CLIENT->HOST bagging REQUEST: the pile or clump a client used a folded bag or a roll on.
        // The host re-tests the target through the sender's own reach token, then spawns the bag
        // and destroys the target itself, so the prop seams carry both. The reach, the type and the
        // rate live in the module; the format lives here.
        // coop::pack_trash_intent::OnPackTrashIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PackTrashIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PackTrashIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PackTrashIntentPayload)) {
            UE_LOGW("event_feed: PackTrashIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PackTrashIntentPayload));
            break;
        }
        net::PackTrashIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.eid == 0) {
            UE_LOGW("event_feed: PackTrashIntent eid==0 -- dropping");
            break;
        }
        if (p.targetKind > 1 || p.toolKind > 1) {
            UE_LOGW("event_feed: PackTrashIntent kinds out of range (target=%u tool=%u) -- dropping",
                    static_cast<unsigned>(p.targetKind), static_cast<unsigned>(p.toolKind));
            break;
        }
        UE_LOGI("[PACK-TRASH] RECEIVED eid=%u slot=%d target=%u tool=%u", p.eid, msg.senderPeerSlot,
                static_cast<unsigned>(p.targetKind), static_cast<unsigned>(p.toolKind));
        coop::pack_trash_intent::OnPackTrashIntent(session, p,
                                                   static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::DroneFlyIntent: {
        // CLIENT->HOST drone call REQUEST: a client pressed the garage console's keyboard. The
        // console is not named -- it is baked into the level and no lane keys it -- so the host
        // resolves the one the sender stands at from its own world and re-tests the lid there,
        // then runs the button's own verb. That resolve, the lid and the rate live in the module;
        // the format lives here.
        // coop::drone_call_intent::OnDroneFlyIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DroneFlyIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: DroneFlyIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::DroneFlyIntentPayload)) {
            UE_LOGW("event_feed: DroneFlyIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DroneFlyIntentPayload));
            break;
        }
        net::DroneFlyIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.verb != 0) {
            UE_LOGW("event_feed: DroneFlyIntent verb=%u has no lane -- dropping",
                    static_cast<unsigned>(p.verb));
            break;
        }
        UE_LOGI("[DRONE-CALL] RECEIVED slot=%d", msg.senderPeerSlot);
        coop::drone_call_intent::OnDroneFlyIntent(session, p,
                                                  static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::KerfusIntent: {
        // CLIENT->HOST a Kerfus verb its gate refused: on/off, fix the servers, pat. The host resolves
        // the Kerfus by its prop eid within the sender's reach and runs the same verb on its copy; the
        // resolve, the reach and the rate live in the module, the format here. coop::kerfus_intent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfusIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: KerfusIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfusIntentPayload)) {
            UE_LOGW("event_feed: KerfusIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfusIntentPayload));
            break;
        }
        net::KerfusIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfus_intent::OnIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::BroomStroke: {
        // CLIENT->HOST broom stroke. The client refused its own stroke at the montage notify and
        // sent what it read of its holder; the host runs the game's stroke on its mirror of that
        // client's broom with those reads answered. coop::broom_stroke::OnBroomStroke.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: BroomStroke received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: BroomStroke from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::BroomStrokePayload)) {
            UE_LOGW("event_feed: BroomStroke payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BroomStrokePayload));
            break;
        }
        net::BroomStrokePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Strict on format: a stroke carrying any non-finite value is refused whole.
        const float vals[] = {p.startX, p.startY, p.startZ, p.endX, p.endY, p.endZ,
                              p.fwdX,   p.fwdY,   p.fwdZ,   p.velX, p.velY, p.velZ};
        bool finite = true;
        for (float v : vals) finite = finite && std::isfinite(v);
        if (!finite) {
            UE_LOGW("event_feed: BroomStroke with a non-finite value from slot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        coop::broom_stroke::OnBroomStroke(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::ThrowIntent: {       // CLIENT->HOST throw of a puppet-held clump
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
    case net::ReliableKind::PileResyncRequest:  // STAGED -- ID reserved, no handler yet
        UE_LOGI("event_feed: STAGED ReliableKind=%u from slot=%d -- no handler yet",
                static_cast<unsigned>(msg.kind), msg.senderPeerSlot);
        break;
    case net::ReliableKind::PropDropIntent: {   // CLIENT->HOST -- client placed a keyed world
                                                // prop it had picked up; the HOST re-spawns it
                                                // authoritatively by Key + broadcasts.
                                                // Host-authoritative (the GrabIntent shape).
                                                // coop::prop_drop_intent.
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
    case net::ReliableKind::ReelEjectIntent: {  // CLIENT->HOST -- a caddy/reelbox eject birthed a
                                                // reel prop in the client's hands; the HOST authors
                                                // it (class-whitelisted to the reel lineage).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ReelEjectIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ReelEjectIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PropDropIntentPayload)) {
            UE_LOGW("event_feed: ReelEjectIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropDropIntentPayload));
            break;
        }
        net::PropDropIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::prop_drop_intent::OnReelEjectIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::RoachConsumed: {  // CLIENT->HOST -- a native eat/stomp destroyed a
                                              // roach component locally; the host deletes its
                                              // nearest roach and the next RoachState converges
                                              // every peer. Role/sender validation lives in
                                              // roach_sync::OnConsumedIntent.
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
