// coop/dispatch/event_dispatch_intent.cpp -- the CLIENT->HOST INTENT / REQUEST reliable-kind case
// bodies: a client asks the HOST to perform an action it alone is authoritative for. The switch
// below is the family's membership declaration; nothing re-lists it.
//
// The intent family is a distinct concept from keyed device-STATE mirrors. MOST cases are
// CLIENT->HOST and gate on role()==Host plus a client sender slot (1..kMaxPeers-1) at the
// trust boundary before handing the request to the authoritative module -- but not all of
// them, so read the case: GrabRefused, RemoverResult and PropActReply are HOST->CLIENT answers,
// EraserPressIntent, DeskVerb and DeskPingVerdict carry the host's answer as well, and RoachConsumed
// defers both gates to roach_sync::OnConsumedIntent. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family. The economy kinds (orders, coin gun, upgrades, point
// sacks) live in event_dispatch_economy.cpp.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/creature_hit_intent.h"  // CLIENT->HOST a swing that hit a creature's mirror
#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_convert_host.h"
#include "coop/creatures/roach_sync.h"    // CLIENT->HOST local roach consumption intent
#include "coop/interactables/atv_authority.h"  // CLIENT->HOST an ATV seat or takeover request, judged by the host
#include "coop/interactables/camera_toggle_intent.h"  // CLIENT->HOST use of a security camera, or its laptop light button
#include "coop/interactables/door_verb_intent.h"  // CLIENT->HOST press, hit or pry of a base door
#include "coop/interactables/sleepingbag_sync.h"  // CLIENT->HOST fold/unfold; HOST runs the source Blueprint
#include "coop/interactables/keypad_verbs.h"  // CLIENT->HOST digit, submit, cancel, keycard, reset
#include "coop/interactables/laptop_tab.h"  // CLIENT->HOST the PC user's tab
#include "coop/interactables/drone_call_intent.h"  // CLIENT->HOST press of the drone console
#include "coop/interactables/eraser_press_intent.h"  // CLIENT->HOST press of the drive eraser; HOST->CLIENT its show
#include "coop/creatures/kerfus_intent.h"  // CLIENT->HOST a Kerfus verb (on/off, fix the servers, pat)
#include "coop/interactables/desk_ping_sync.h"    // a ping's verdict on the main desk, and its answers
#include "coop/interactables/desk_verb_intent.h"  // a press on the main desk's save family, and its answers
#include "coop/element/registry.h"  // IsAllowedHostAllocatedEid
#include "coop/interactables/coffee_intent.h"
#include "coop/props/pack_trash_intent.h"  // CLIENT->HOST bagging of a pile or clump
#include "coop/props/prop_act_host.h"  // CLIENT->HOST a take or use of a shared prop, and its commit
#include "coop/props/prop_act_request.h"  // HOST->CLIENT its answer
#include "coop/props/prop_drop_intent.h"  // CLIENT->HOST client-placed keyed prop
#include "coop/props/remover_request.h"  // CLIENT->HOST the tool gun's remover; HOST->CLIENT its answer
#include "coop/items/broom_stroke.h"
#include "coop/world/explosion_request.h"  // CLIENT->HOST a shot and an explosion request, strictly checked and queued
#include "coop/world/salt_rocket_ignite.h"  // CLIENT->HOST a light on a salt rocket, judged and performed on the host
#include "coop/props/trash_channel.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"  // ue_wrap::FVector (ThrowIntent camFwd)

#include <cmath>
#include <cstring>
#include <string>

namespace coop::event_feed {

bool HandleIntentEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
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
    case net::ReliableKind::SleepingbagIntent: {
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: SleepingbagIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: SleepingbagIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::SleepingbagIntentPayload)) {
            UE_LOGW("event_feed: SleepingbagIntent payload size %zu != %zu",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SleepingbagIntentPayload));
            break;
        }
        net::SleepingbagIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::sleepingbag_sync::OnIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::CameraToggleIntent: {
        // CLIENT->HOST: a client used a security camera or pressed the laptop's camera-light button,
        // and the host sets the bit to the value that client wanted. The camera's resolve, the reach
        // and the rate live in the module; the format lives here.
        // coop::camera_toggle_intent::OnCameraToggleIntent.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: CameraToggleIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CameraToggleIntent from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::CameraToggleIntentPayload)) {
            UE_LOGW("event_feed: CameraToggleIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::CameraToggleIntentPayload));
            break;
        }
        net::CameraToggleIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: one of the two bits, and a value that is a bool.
        if (p.bit > net::camera_toggle::kLight || p.desired > 1) {
            UE_LOGW("event_feed: CameraToggleIntent bit=%u desired=%u out of range -- dropping",
                    static_cast<unsigned>(p.bit), static_cast<unsigned>(p.desired));
            break;
        }
        coop::camera_toggle_intent::OnCameraToggleIntent(session, p,
                                                         static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::LaptopTabReport: {
        // CLIENT->HOST: the peer holding the PC's claim switched the laptop widget's tab. Whether the sender holds
        // the claim, the index against the host's own switcher and the pace live in the module; the format lives
        // here. coop::laptop_tab::OnReport.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: LaptopTabReport received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: LaptopTabReport from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::LaptopTabPayload)) {
            UE_LOGW("event_feed: LaptopTabReport payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::LaptopTabPayload));
            break;
        }
        net::LaptopTabPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::laptop_tab::OnReport(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
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
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        // Both refusals are the lane's to say, in its tally: a sender cannot make the router's log the cost.
        if (msg.payloadLen < sizeof(net::KerfurConvertPayload)) {
            coop::kerfur_convert_host::NoteRefused(senderSlot, "payload too short");
            break;
        }
        net::KerfurConvertPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.toProp != 0 && p.toProp != 1) {
            coop::kerfur_convert_host::NoteRefused(senderSlot, "toProp out of range");
            break;
        }
        coop::kerfur_convert_host::OnConvertRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurCommand: {
        // client->host kerfur radial-menu command (follow/idle/patrol/...). Host-authoritative; the
        // handler waits for the sender's body, validates the element + kerfur class + the BP guards,
        // then runs the verb with the kerfur serving the REQUESTING player (senderSlot).
        // coop::kerfur_command.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfurCommand received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        if (msg.payloadLen < sizeof(net::KerfurCommandPayload)) {
            coop::kerfur_command::NoteRefused(senderSlot, "payload too short");
            break;
        }
        net::KerfurCommandPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfur_command::OnCommandRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::GrabRefused: {
        // HOST->CLIENT, one recipient, never relayed: the answer to a GrabIntent the host did not
        // perform, or (reason HoldEnded) the notice that the host ended the recipient's carry.
        // coop::trash_channel::OnGrabRefused.
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
    case net::ReliableKind::CoffeeActionIntent: {
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CoffeeActionIntent dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        net::CoffeeActionIntentPayload p{};
        if (msg.payloadLen != sizeof(p)) { coop::coffee_intent::CountDropped(coop::coffee_intent::Drop::Size); break; }
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > sizeof(p.key.data)) { coop::coffee_intent::CountDropped(coop::coffee_intent::Drop::Key); break; }
        coop::coffee_intent::OnAction(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::RemoverRequest: {
        // CLIENT->HOST: a client pressed the tool gun's remover. The host judges the sender's right, the element, who
        // holds it and whether the sender could see it, and removes the prop itself (coop::remover_request). Never
        // relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: RemoverRequest received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        // The refusal is the lane's to say, in its tally: a sender cannot make the router's log the cost.
        if (msg.payloadLen < sizeof(net::RemoverRequestPayload)) {
            coop::remover_request::NoteRefused(senderSlot, "payload too short");
            break;
        }
        net::RemoverRequestPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::remover_request::OnRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::RemoverResult: {
        // HOST->CLIENT: what the host did with this client's RemoverRequest (coop::remover_request shows the five
        // visible reasons).
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::RemoverResultPayload)) {
            UE_LOGW("event_feed: RemoverResult dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::RemoverResultPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::remover_request::OnResult(p);
        break;
    }
    case net::ReliableKind::CreatureHitIntent: {
        // CLIENT->HOST: a client's swing hit a creature's mirror. The host judges the swing and runs the verb on its
        // own creature (coop::creature_hit_intent); the format lives here. Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: CreatureHitIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: CreatureHitIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::CreatureHitIntentPayload)) {
            UE_LOGW("event_feed: CreatureHitIntent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::CreatureHitIntentPayload));
            break;
        }
        net::CreatureHitIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: a known verb, a host-allocated id, a damage that is not negative, and every float a
        // finite value within the world's range.
        constexpr float kMaxComponent = 1e7f;
        const auto bounded = [](float v) { return std::isfinite(v) && std::fabs(v) <= kMaxComponent; };
        bool floatsOk = bounded(p.damage) && p.damage >= 0.f;
        for (int i = 0; i < 3; ++i) floatsOk = floatsOk && bounded(p.hitLoc[i]) && bounded(p.impact[i]);
        if (p.verb >= net::kCreatureHitVerbCount || !floatsOk ||
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.creatureEid)) {
            UE_LOGW("event_feed: CreatureHitIntent verb=%u eid=%u out of range -- dropping",
                    static_cast<unsigned>(p.verb), p.creatureEid);
            break;
        }
        coop::creature_hit_intent::OnIntent(static_cast<int>(msg.senderPeerSlot), p);
        break;
    }
    case net::ReliableKind::ProjectileFired: {
        // CLIENT->HOST: a shot this client's launcher fired. The host keeps it for the blast that names it
        // (coop::explosion_request); the format lives there, counted. Never relayed. The handler refuses a sender
        // that is not world-ready, as MTA tests IsJoined
        // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2957).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ProjectileFired received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ProjectileFired from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        coop::explosion_request::OnFiredReceived(static_cast<int>(msg.senderPeerSlot), msg.payload, msg.payloadLen);
        break;
    }
    case net::ReliableKind::ExplosionRequest: {
        // CLIENT->HOST: an explosion this client's player caused was dissolved here. The host judges it and
        // performs it natively (coop::explosion_request); the format lives there, counted. Never relayed. The handler
        // refuses a sender that is not world-ready, as MTA tests IsJoined
        // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2835).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ExplosionRequest received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ExplosionRequest from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        coop::explosion_request::OnRequestReceived(static_cast<int>(msg.senderPeerSlot), msg.payload, msg.payloadLen);
        break;
    }
    case net::ReliableKind::AtvExplodeRequest: {
        // CLIENT->HOST: the ATV this client syncs exploded here and its blast was dissolved. The host judges the
        // sender is the ATV's syncer and performs the blast (coop::explosion_request); the format lives there,
        // counted. Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: AtvExplodeRequest received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: AtvExplodeRequest from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        coop::explosion_request::OnAtvRequestReceived(static_cast<int>(msg.senderPeerSlot), msg.payload,
                                                      msg.payloadLen);
        break;
    }
    case net::ReliableKind::PropIgniteIntent: {
        // CLIENT->HOST: this client's player lit a salt rocket its own copy refused to run. The host judges the light
        // and performs it on its own copy (coop::salt_rocket_ignite); the format lives there, counted. Never relayed.
        // The handler refuses a sender that is not world-ready, as MTA tests IsJoined
        // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2835).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PropIgniteIntent received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PropIgniteIntent from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        coop::salt_rocket_ignite::OnIntentReceived(static_cast<int>(msg.senderPeerSlot), msg.payload, msg.payloadLen);
        break;
    }
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
    case net::ReliableKind::PropActRequest: {
        // CLIENT->HOST: a client asks to take or use a shared prop before its verb runs. The host judges it, and
        // answers a grant or a refusal (coop::prop_act_host). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PropActRequest received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot = (msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers)
                                       ? static_cast<uint8_t>(msg.senderPeerSlot)
                                       : static_cast<uint8_t>(0xFF);
        // The refusal is the lane's to say, in its tally: a sender cannot make the router's log the cost.
        if (senderSlot == 0xFF) {
            coop::prop_act_host::NoteBadSender();
            break;
        }
        if (msg.payloadLen < sizeof(net::PropActRequestPayload)) {
            coop::prop_act_host::NoteShortPayload(senderSlot);
            break;
        }
        net::PropActRequestPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::prop_act_host::OnRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::PropActCommit: {
        // CLIENT->HOST: the client's verb on a granted prop ran or did not. On a commit the host destroys the prop
        // itself and tells the other peers (coop::prop_act_host). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PropActCommit received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot = (msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers)
                                       ? static_cast<uint8_t>(msg.senderPeerSlot)
                                       : static_cast<uint8_t>(0xFF);
        if (senderSlot == 0xFF) {
            coop::prop_act_host::NoteBadSender();
            break;
        }
        if (msg.payloadLen < sizeof(net::PropActCommitPayload)) {
            coop::prop_act_host::NoteShortPayload(senderSlot);
            break;
        }
        net::PropActCommitPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::prop_act_host::OnCommit(p, senderSlot);
        break;
    }
    case net::ReliableKind::PropActReply: {
        // HOST->CLIENT: the host's answer to this client's take or use of a shared prop: a grant replays the verb, a
        // refusal shows a line (coop::prop_act_request). Never relayed.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::PropActReplyPayload)) {
            UE_LOGW("event_feed: PropActReply dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::PropActReplyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::prop_act_request::OnReply(p);
        break;
    }
    case net::ReliableKind::EraserPressIntent: {
        // CLIENT->HOST: a client pressed the drive eraser's delete button; the host tests the sender's reach, waits for
        // its slot to hold the named drive and presses it. HOST->CLIENT: what the host's eraser did, for this peer's
        // eraser to show (coop::eraser_press_intent::OnEraserPressIntent).
        const bool fromClient = session.role() == net::Role::Host && msg.senderPeerSlot >= 1 &&
                                msg.senderPeerSlot < net::kMaxPeers;
        const bool fromHost = session.role() == net::Role::Client && msg.senderPeerSlot == 0;
        if ((!fromClient && !fromHost) || msg.payloadLen < sizeof(net::EraserPressIntentPayload)) {
            UE_LOGW("event_feed: EraserPressIntent dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::EraserPressIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::eraser_press_intent::OnEraserPressIntent(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::DroneFlyIntent: {
        // CLIENT->HOST drone request: a client pressed the keyboard or leave-timer face. The
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
        if (p.verb > net::drone_verb::kMax) {
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
    case net::ReliableKind::DeskVerb: {
        // CLIENT->HOST a press on the desk's save family its gate refused; HOST->that CLIENT the verdict
        // and the press's own effects for the presser's machine. The identity, the reach and the rate live
        // in the module, the format here. coop::desk_verb_intent.
        if (msg.payloadLen < sizeof(net::DeskVerbPayload)) {
            UE_LOGW("event_feed: DeskVerb payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskVerbPayload));
            break;
        }
        net::DeskVerbPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::desk_verb_intent::OnMessage(session, p, msg.senderPeerSlot);
        break;
    }
    case net::ReliableKind::DeskPingVerdict: {
        // CLIENT->HOST the verdict of a ping its gate refused; HOST->that CLIENT a refusal or the find. The
        // priming, the refusals and the answers live in the module, the format here. coop::desk_ping_sync.
        if (msg.payloadLen < sizeof(net::DeskPingVerdictPayload)) {
            UE_LOGW("event_feed: DeskPingVerdict payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskPingVerdictPayload));
            break;
        }
        net::DeskPingVerdictPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::desk_ping_sync::OnMessage(session, p, msg.senderPeerSlot);
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
    case net::ReliableKind::PileResyncRequest:  // STAGED -- ID reserved, no sender or handler yet; the
                                                // host's receive edge refuses it until one is built
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
