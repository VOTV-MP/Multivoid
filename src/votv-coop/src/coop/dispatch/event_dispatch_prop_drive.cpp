// coop/dispatch/event_dispatch_prop_drive.cpp -- the driven prop's reliable kinds: PropDriveEnd (the
// close of a prop's stream, the host's to all or a syncing client's to the host) and the prop
// syncer's pair, PropSyncClaim (client to host) and PropSyncAnswer (host to one client), the host's refusal of
// a second hold of one prop (PropHoldRefused, host to every client), a garbage container's state (GarbageBinState, host
// to all), a container's break or a saw's cut asked of the host (ContainerBreakRequest), a plank's, radiator's or
// security TV's pry asked of the host (PryAsk) and its state back to every client (PryState), a found dingus (DingusReport,
// client to host; DingusFound, host to every client), and the developer
// filler DevLoadFiller (host to one client), DevParityReport (a joiner's pose report to the host) and
// DevContainerParity (a joiner's container content report to the host). Each case
// validates at the trust boundary and hands off to its module. The switch below is the family's
// membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/ambient_creature_sync.h"
#include "coop/creatures/walker_gaze_host.h"
#include "coop/creatures/walker_state_client.h"
#include "coop/world/ambient_fixture_sync.h"
#include "coop/dev/container_parity.h"     // the container join drill's report receiver
#include "coop/dev/prop_syncer_drill.h"    // the drill's filler receiver
#include "coop/dev/pile_defer_report.h"    // the pile deferral drill's report receiver
#include "coop/element/registry.h"
#include "coop/props/roomba_sync.h"  // a robot cleaner's switch, the host's
#include "coop/world/plant_edit_gate.h"
#include "coop/world/plant_sync.h"
#include "coop/world/flood_sync.h"
#include "coop/interactables/woodchipper_sync.h"
#include "coop/interactables/atv_host_judge.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/props/prop_drive_stream.h"  // the driven-prop end edge
#include "coop/props/prop_hold_claim.h"    // the client's receiver of a refused hold
#include "coop/props/miner_bounds.h"  // what a mining rig can hold
#include "coop/props/miner_sync.h"   // a GPU mining rig's counts and points, the host's
#include "coop/props/dingus_sync.h"  // a found dingus: a client's report, the host's word
#include "coop/props/prop_food_sync.h"     // a world food's condition and a client's bite
#include "coop/props/prop_pry_host.h"      // a client's pry ask, judged by the host
#include "coop/props/prop_pry_sync.h"      // the host's pried-prop state, applied by a client
#include "coop/player/hand_record_sync.h"  // a held item's own record on its hand mirrors
#include "coop/world/cover_sync.h"  // the desk's back plates and the stations' doors
#include "coop/props/fireext_sync.h"  // an extinguisher's thrust and a player's spray
#include "coop/props/prop_syncer.h"        // the client's answer receiver
#include "coop/props/prop_syncer_host.h"   // the host's claim judge and a client's End

#include "coop/interactables/garbage_bin_sync.h"
#include "coop/props/container_break.h"
#include "coop/world/spill_sync.h"
#include "coop/interactables/car_charger_sync.h"
#include "coop/world/sus_hole_intent.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/atv.h"   // ClampCharge: a host state's battery

#include <cmath>
#include <cstring>

namespace coop::event_feed {

namespace {

// An extinguisher message's fields, checked before any lane sees them. A State is op 0 (a thrust) or op 1 (a spray); an
// Intent is op 2 (a spray's start or stop, with the hand's charge) or op 3 (a runaway ask: only `eid` is set).
bool FireExtWellFormed(bool intent, const net::FireExtPayload& p) {
    if (!std::isfinite(p.life) || p.life < -0.01f || p.life > 1.01f) return false;
    if (!intent) return p.op <= net::kFireExtOpSpray && p.on <= 1 && p.charged <= 1 && p.slot < net::kMaxPeers;
    if (p.op == net::kFireExtOpSprayAsk) return p.on <= 1;
    return p.op == net::kFireExtOpRunawayAsk && p.slot == 0 && p.on == 0 && p.charged == 0 && p.life == 0.f;
}

// A mining rig message's fields, checked before any lane sees them. A State is op 0 (a rig's numbers, `pad` its powered flag) or
// op 1 (the answer to an ask: `pad` the reason, `granted` equal to it being 0, the numbers unset, the item 0..3). An Intent is
// op 2 (a use: the item 0..2) or op 3 (a claim: no item); an intent carries nothing else.
bool MinerWellFormed(bool intent, const net::MinerPayload& p) {
    if (intent) {
        const bool numbersUnset = p.gpus == 0 && p.slots == 0 && p.fans == 0 && p.points == 0 && p.overheat == 0.f;
        if (!numbersUnset || p.granted != 0 || p.pad != 0) return false;
        if (p.op == net::kMinerOpAdd) return p.item <= net::kMinerItemFan;
        return p.op == net::kMinerOpClaim && p.item == 0;
    }
    if (p.op == net::kMinerOpState)
        return p.item == 0 && p.granted == 0 && p.pad <= 1 &&
               ::coop::miner_sync::InBounds(p.gpus, p.slots, p.fans, p.points, p.overheat);
    if (p.op != net::kMinerOpAnswer) return false;
    const bool numbersUnset = p.gpus == 0 && p.slots == 0 && p.fans == 0 && p.points == 0 && p.overheat == 0.f;
    return numbersUnset && p.item <= net::kMinerItemClaim && p.pad < net::kMinerReasonBudget &&
           p.granted == (p.pad == net::kMinerReasonGranted ? 1 : 0);
}

// Every mining rig message the dispatcher drops (the wrong way, the wrong length or out of range) is counted on one counter,
// logged at the first and each power of two.
void DropMiner(const net::Session::ReliableMessage& msg) {
    static uint64_t s_dropped = 0;
    const uint64_t n = ++s_dropped;
    if ((n & (n - 1)) == 0)
        UE_LOGW("event_feed: a mining rig message dropped (the wrong way, of %zu bytes or out of range), from slot %d: %llu so far",
                static_cast<size_t>(msg.payloadLen), msg.senderPeerSlot, static_cast<unsigned long long>(n));
}

// Every basement flood message the dispatcher drops (the wrong way, the wrong length or malformed) is counted on one
// counter, logged at the first and each power of two.
void DropFlood(const net::Session::ReliableMessage& msg) {
    static uint64_t s_dropped = 0;
    const uint64_t n = ++s_dropped;
    if ((n & (n - 1)) == 0)
        UE_LOGW("event_feed: a flood message dropped (the wrong way, of %zu bytes or malformed), from slot %d: %llu so far",
                static_cast<size_t>(msg.payloadLen), msg.senderPeerSlot, static_cast<unsigned long long>(n));
}

// Every extinguisher message the dispatcher drops (the wrong way, the wrong length or out of range) is counted on one
// counter, logged at the first and each power of two.
void DropFireExt(const net::Session::ReliableMessage& msg) {
    static uint64_t s_dropped = 0;
    const uint64_t n = ++s_dropped;
    if ((n & (n - 1)) == 0)
        UE_LOGW("event_feed: an extinguisher message dropped (the wrong way, of %zu bytes or out of range), from slot %d: "
                "%llu so far", static_cast<size_t>(msg.payloadLen), msg.senderPeerSlot,
                static_cast<unsigned long long>(n));
}

}  // namespace

bool HandlePropDriveEvent(net::Session& session,
                          const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::PropDriveEnd: {
        // A driven prop's stream closed: the host's to all, or a syncing client's to the host. Never
        // relayed; validated here and applied by the stream's receiver on the game thread.
        if (msg.payloadLen < sizeof(net::PropDriveEndPayload)) {
            UE_LOGW("event_feed: PropDriveEnd payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropDriveEndPayload));
            break;
        }
        net::PropDriveEndPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const float vals[12] = { p.x, p.y, p.z, p.pitch, p.yaw, p.roll,
                                 p.linVelX, p.linVelY, p.linVelZ, p.angVelX, p.angVelY, p.angVelZ };
        bool finite = true;
        for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
        if (!finite) { UE_LOGW("event_feed: PropDriveEnd floats non-finite -- dropping"); break; }
        if (std::fabs(p.x) > net::kMaxCoord || std::fabs(p.y) > net::kMaxCoord ||
            std::fabs(p.z) > net::kMaxCoord) {
            UE_LOGW("event_feed: PropDriveEnd location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                    p.x, p.y, p.z);
            break;
        }
        if (p.eid == 0u || p.eid == coop::element::kInvalidId ||
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.eid)) {
            UE_LOGW("event_feed: PropDriveEnd eid=0x%08x not a valid host-allocated id -- dropping", p.eid);
            break;
        }
        if (session.role() == net::Role::Host) {
            // A client's End for a prop it syncs: the host judges it against the row's syncer and
            // generation, and re-publishes it; the stream's receiver is the client side.
            if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
                UE_LOGW("event_feed: PropDriveEnd from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
                break;
            }
            ::coop::prop_syncer_host::OnClientEnd(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
            break;
        }
        ::coop::prop_syncer::OnHostEnd(p.eid, p.gen);   // the entry of the generation the host ended goes
        ::coop::prop_drive_stream::OnEnd(p);   // the router is on the game thread; PropRelease's shape
        break;
    }
    case net::ReliableKind::PropSyncClaim: {
        // CLIENT->HOST: this client's interaction moves a prop and it asks to be its syncer. The host
        // judges the claim and answers it (coop::prop_syncer_host::OnClaim). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: PropSyncClaim received on a client -- dropping");
            break;
        }
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PropSyncClaim from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PropSyncClaimPayload)) {
            UE_LOGW("event_feed: PropSyncClaim payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropSyncClaimPayload));
            break;
        }
        net::PropSyncClaimPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > sizeof(p.key.data) || p.eid == 0u ||
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.eid)) {
            UE_LOGW("event_feed: PropSyncClaim eid=0x%08x key.len=%u malformed -- dropping", p.eid,
                    static_cast<unsigned>(p.key.len));
            break;
        }
        ::coop::prop_syncer_host::OnClaim(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::PropSyncAnswer: {
        // HOST->CLIENT: the host's verdict on a claim, or the end of a row it granted. Host-authored: a
        // client's copy is refused at the host's receive edge (client_authorship.h).
        if (msg.payloadLen < sizeof(net::PropSyncAnswerPayload)) {
            UE_LOGW("event_feed: PropSyncAnswer payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropSyncAnswerPayload));
            break;
        }
        net::PropSyncAnswerPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.eid == 0u || p.result < static_cast<uint8_t>(net::PropSyncResult::Granted) ||
            p.result > static_cast<uint8_t>(net::PropSyncResult::Refused) ||
            p.refusal > static_cast<uint8_t>(net::PropSyncRefusal::Paced)) {
            UE_LOGW("event_feed: PropSyncAnswer eid=0x%08x result=%u refusal=%u malformed -- dropping", p.eid,
                    static_cast<unsigned>(p.result), static_cast<unsigned>(p.refusal));
            break;
        }
        ::coop::prop_syncer::OnAnswer(p);
        break;
    }
    case net::ReliableKind::PropHoldRefused: {
        // HOST->CLIENTS: the host kept another hold of a prop and refused this one. Host-authored: a client's copy
        // is refused at the host's receive edge (client_authorship.h).
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: PropHoldRefused received on the host -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::PropHoldRefusedPayload)) {
            UE_LOGW("event_feed: PropHoldRefused payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropHoldRefusedPayload));
            break;
        }
        net::PropHoldRefusedPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > sizeof(p.key.data) || p.holdGen == 0 || p.slot < 1 || p.slot >= net::kMaxPeers ||
            p.holder >= net::kMaxPeers || p.holder == p.slot) {
            UE_LOGW("event_feed: PropHoldRefused slot=%u holder=%u hold=%u key.len=%u malformed -- dropping",
                    static_cast<unsigned>(p.slot), static_cast<unsigned>(p.holder),
                    static_cast<unsigned>(p.holdGen), static_cast<unsigned>(p.key.len));
            break;
        }
        ::coop::prop_hold_claim::OnRefused(p);
        break;
    }
    case net::ReliableKind::FoodState: {
        // HOST->ALL: refused from a client at the host's edge (client_authorship).
        if (msg.payloadLen < sizeof(net::FoodStatePayload)) {
            UE_LOGW("event_feed: FoodState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::FoodStatePayload));
            break;
        }
        net::FoodStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || !std::isfinite(p.temperature) ||
            !std::isfinite(p.ripeness) || !std::isfinite(p.poisonDelay) || !std::isfinite(p.poisonStrength)) {
            UE_LOGW("event_feed: FoodState malformed -- dropping");
            break;
        }
        ::coop::props::prop_food_sync::OnState(p);
        break;
    }
    case net::ReliableKind::FoodBite: {
        // CLIENT->HOST: never relayed. The host's lane judges it.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: FoodBite from slot %d on this role -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::FoodBitePayload)) {
            UE_LOGW("event_feed: FoodBite payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::FoodBitePayload));
            break;
        }
        net::FoodBitePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data)) {
            UE_LOGW("event_feed: FoodBite malformed -- dropping");
            break;
        }
        ::coop::props::prop_food_sync::OnBite(msg.senderPeerSlot, p);
        break;
    }
    case net::ReliableKind::FoodRelease: {
        // CLIENT->HOST: never relayed. The host's lane judges it.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: FoodRelease from slot %d on this role -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::FoodStatePayload)) {
            UE_LOGW("event_feed: FoodRelease payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::FoodStatePayload));
            break;
        }
        net::FoodStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || !std::isfinite(p.temperature) ||
            !std::isfinite(p.ripeness)) {
            UE_LOGW("event_feed: FoodRelease malformed -- dropping");
            break;
        }
        ::coop::props::prop_food_sync::OnRelease(msg.senderPeerSlot, p);
        break;
    }
    case net::ReliableKind::FoodHand: {
        // ANY->ALL: a client's copy is relayed at the host's receive edge (session_lanes.h).
        if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: FoodHand from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::FoodHandPayload)) {
            UE_LOGW("event_feed: FoodHand payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::FoodHandPayload));
            break;
        }
        net::FoodHandPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (!std::isfinite(p.temperature) || !std::isfinite(p.ripeness)) {
            UE_LOGW("event_feed: FoodHand malformed -- dropping");
            break;
        }
        if (p.uses < 0) {  // every receiver judges it; the host has relayed it already
            ::coop::props::prop_food_sync::OnBadHand(msg.senderPeerSlot);
            break;
        }
        ::coop::props::prop_food_sync::OnHand(msg.senderPeerSlot, p);
        break;
    }
    case net::ReliableKind::HandRecord: {
        // Any peer to the host, the host to all (coop/player/hand_record_sync); chunked.
        if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: HandRecord from invalid senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) break;
        net::BlobChunkPayload c{};
        std::memcpy(&c, msg.payload, sizeof(c));
        ::coop::hand_record_sync::OnChunk(c, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::CoverState:
    case net::ReliableKind::CoverIntent: {
        // CoverState HOST->ALL (op 5 to one asker), CoverIntent CLIENT->HOST (coop/world/cover_sync). Exact length.
        const bool intent = msg.kind == net::ReliableKind::CoverIntent;
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::CoverPayload)) {
            UE_LOGW("event_feed: a cover message the wrong way or of %zu bytes from slot %d -- dropping",
                    static_cast<size_t>(msg.payloadLen), msg.senderPeerSlot);
            break;
        }
        net::CoverPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (intent) ::coop::cover_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::cover_sync::OnState(p);
        break;
    }
    case net::ReliableKind::FireExtState:
    case net::ReliableKind::FireExtIntent: {
        // FireExtState HOST->ALL, FireExtIntent CLIENT->HOST (coop/props/fireext_sync). Exact length.
        const bool intent = msg.kind == net::ReliableKind::FireExtIntent;
        net::FireExtPayload p{};
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::FireExtPayload)) {
            DropFireExt(msg);
            break;
        }
        std::memcpy(&p, msg.payload, sizeof(p));
        if (!FireExtWellFormed(intent, p)) {
            DropFireExt(msg);
            break;
        }
        if (intent) ::coop::fireext_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::fireext_sync::OnState(p);
        break;
    }
    case net::ReliableKind::MinerState:
    case net::ReliableKind::MinerIntent: {
        // MinerState HOST->ALL (op 1 to one asker), MinerIntent CLIENT->HOST (coop/props/miner_sync). Exact length.
        const bool intent = msg.kind == net::ReliableKind::MinerIntent;
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::MinerPayload)) {
            DropMiner(msg);
            break;
        }
        net::MinerPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (!MinerWellFormed(intent, p)) {
            DropMiner(msg);
            break;
        }
        if (intent) ::coop::miner_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::miner_sync::OnState(p);
        break;
    }
    case net::ReliableKind::DingusFound:
    case net::ReliableKind::DingusReport: {
        // DingusFound HOST->ALL, DingusReport CLIENT->HOST (coop/props/dingus_sync). Exact length, then a well-formed key.
        const bool report = msg.kind == net::ReliableKind::DingusReport;
        static_assert(sizeof(net::DingusFoundPayload) == sizeof(net::DingusReportPayload));
        if (report != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::DingusReportPayload)) {
            ::coop::dingus_sync::CountDropped(msg.senderPeerSlot, msg.payloadLen);
            break;
        }
        net::WireKey key{};
        std::memcpy(&key, msg.payload, sizeof(key));
        if (!::coop::dingus_sync::WireKeyWellFormed(key)) {
            ::coop::dingus_sync::CountDropped(msg.senderPeerSlot, msg.payloadLen);
            break;
        }
        if (report) {
            net::DingusReportPayload p{};
            p.key = key;
            ::coop::dingus_sync::OnReport(msg.senderPeerSlot, p);
        } else {
            net::DingusFoundPayload p{};
            p.key = key;
            ::coop::dingus_sync::OnFound(p);
        }
        break;
    }
    case net::ReliableKind::PlantState:
    case net::ReliableKind::PlantEdit: {
        // PlantState HOST->ALL, PlantEdit CLIENT->HOST.
        const bool edit = msg.kind == net::ReliableKind::PlantEdit;
        if (edit != (session.role() == net::Role::Host)) {
            UE_LOGW("event_feed: a plant message the wrong way from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PlantStatePayload)) {
            UE_LOGW("event_feed: plant payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PlantStatePayload));
            break;
        }
        net::PlantStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.alive > 1 || p.hideDirt > 1 || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.pitch) ||
            !std::isfinite(p.yaw) || !std::isfinite(p.roll) || !std::isfinite(p.grow) || !std::isfinite(p.fertilizer) ||
            !std::isfinite(p.water) || !std::isfinite(p.fruitGrow) || !std::isfinite(p.dryness)) {
            UE_LOGW("event_feed: plant message malformed -- dropping");
            break;
        }
        if (edit) ::coop::plant_edit_gate::OnEdit(static_cast<uint8_t>(msg.senderPeerSlot), p);
        else ::coop::plant_sync::OnState(p);
        break;
    }
    case net::ReliableKind::RoombaState:
    case net::ReliableKind::RoombaIntent: {
        // RoombaState HOST->ALL, RoombaIntent CLIENT->HOST (coop/props/roomba_sync). Exact length.
        const bool intent = msg.kind == net::ReliableKind::RoombaIntent;
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::RoombaPayload)) {
            UE_LOGW("event_feed: a robot cleaner message the wrong way or of %zu bytes from slot %d -- dropping",
                    static_cast<size_t>(msg.payloadLen), msg.senderPeerSlot);
            break;
        }
        net::RoombaPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.active > 1) {
            UE_LOGW("event_feed: a robot cleaner message malformed -- dropping");
            break;
        }
        if (intent) ::coop::roomba_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::roomba_sync::OnState(p);
        break;
    }
    case net::ReliableKind::FloodState:
    case net::ReliableKind::FloodIntent: {
        // FloodState HOST->ALL, FloodIntent CLIENT->HOST.
        const bool intent = msg.kind == net::ReliableKind::FloodIntent;
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != sizeof(net::FloodPayload)) {
            DropFlood(msg);
            break;
        }
        net::FloodPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.damaged > 1 || p.op > net::kFloodOpFix || !std::isfinite(p.level)) {
            DropFlood(msg);
            break;
        }
        if (intent) ::coop::flood_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::flood_sync::OnState(p);
        break;
    }
    case net::ReliableKind::WoodchipperState:
    case net::ReliableKind::WoodchipperIntent: {
        // WoodchipperState HOST->ALL, WoodchipperIntent CLIENT->HOST.
        const bool intent = msg.kind == net::ReliableKind::WoodchipperIntent;
        if (intent != (session.role() == net::Role::Host)) {
            UE_LOGW("event_feed: a woodchipper message the wrong way from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::WoodchipperPayload)) {
            UE_LOGW("event_feed: woodchipper payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WoodchipperPayload));
            break;
        }
        net::WoodchipperPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.active > 1 || p.op > net::kWoodchipperOpActiveOnly || !std::isfinite(p.fuel)) {
            UE_LOGW("event_feed: woodchipper message malformed -- dropping");
            break;
        }
        if (intent) ::coop::woodchipper_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::woodchipper_sync::OnState(p);
        break;
    }
    case net::ReliableKind::AtvHostState:
    case net::ReliableKind::AtvRequest: {
        // AtvHostState HOST->ALL, AtvRequest CLIENT->HOST.
        const bool request = msg.kind == net::ReliableKind::AtvRequest;
        if (request != (session.role() == net::Role::Host)) {
            UE_LOGW("event_feed: an ATV host message the wrong way from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvHostPayload)) {
            UE_LOGW("event_feed: ATV host payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvHostPayload));
            break;
        }
        net::AtvHostPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // A request's body is the judge's to refuse: a malformed one that still names its ATV is answered.
        if (request) {
            ::coop::atv_host_judge::OnRequest(msg.senderPeerSlot, p);
            break;
        }
        if (!std::isfinite(p.fuel) || !std::isfinite(p.health) || p.moduleCount > sizeof(p.modules)) {
            UE_LOGW("event_feed: ATV host state malformed -- dropping");
            break;
        }
        // The battery is -1 (none fitted) or a finite charge of 0 or more, brought into the field's range (the game's solar
        // charge leaves it a fraction past 100); any other value is not read, the state's other fields are.
        if (p.flags & net::kAtvHostBattery) {
            if (std::isfinite(p.battery) && (p.battery == -1.f || p.battery >= 0.f)) {
                if (p.battery >= 0.f) p.battery = ue_wrap::atv::ClampCharge(p.battery);
            } else {
                UE_LOGW("event_feed: ATV host state carries a battery that is neither -1 nor a charge -- its battery is not read");
                p.flags = static_cast<uint8_t>(p.flags & ~net::kAtvHostBattery);
            }
        }
        ::coop::atv_host_sync::OnState(p);
        break;
    }
    case net::ReliableKind::DevLoadFiller: {
        // HOST->CLIENT, developer only: one message of the prop syncer drill's load arm. A client never sends it;
        // the drill drops it at once when its row is off.
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: DevLoadFiller received on the host -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::DevLoadFillerPayload)) {
            UE_LOGW("event_feed: DevLoadFiller payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DevLoadFillerPayload));
            break;
        }
        net::DevLoadFillerPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        ::coop::dev::prop_syncer_drill::OnFiller(p.seq, p.total);
        break;
    }
    case net::ReliableKind::DevParityReport: {
        // CLIENT->HOST, developer only: one chunk of a joiner's pose report for the pile deferral drill. Refused on a
        // client; the report module drops it at once when the drill row is off.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DevParityReport received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: DevParityReport payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        ::coop::dev::pile_defer_report::OnChunk(chunk, slot);
        break;
    }
    case net::ReliableKind::DevContainerParity: {
        // CLIENT->HOST, developer only: one chunk of a joiner's container content report for the container join
        // drill. Refused on a client; the report module drops it at once until the drill enables it.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DevContainerParity received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: DevContainerParity payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        ::coop::dev::container_parity::OnChunk(chunk, slot);
        break;
    }
    case net::ReliableKind::SpillState:
    case net::ReliableKind::SpillIntent: {
        // SpillState HOST->ALL, SpillIntent CLIENT->HOST.
        const bool intent = msg.kind == net::ReliableKind::SpillIntent;
        if (intent != (session.role() == net::Role::Host)) {
            UE_LOGW("event_feed: a spill message the wrong way from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::SpillPayload)) {
            UE_LOGW("event_feed: spill payload too short (%zu < %zu)", static_cast<size_t>(msg.payloadLen),
                    sizeof(net::SpillPayload));
            break;
        }
        net::SpillPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (intent) ::coop::spill_sync::OnIntent(msg.senderPeerSlot, p);
        else ::coop::spill_sync::OnState(p);
        break;
    }
    case net::ReliableKind::SpillPour:
    case net::ReliableKind::SpillPourIntent: {
        // SpillPour HOST->ALL, SpillPourIntent CLIENT->HOST.
        const bool intent = msg.kind == net::ReliableKind::SpillPourIntent;
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen < sizeof(net::SpillPourPayload)) {
            UE_LOGW("event_feed: a malformed or misdirected pour from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        net::SpillPourPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (intent) ::coop::spill_sync::OnPourIntent(msg.senderPeerSlot, p);
        else ::coop::spill_sync::OnPour(p);
        break;
    }
    case net::ReliableKind::AmbientTreeState: {
        // HOST->ALL.
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: AmbientTreeState from slot %d -- dropping (host-authored)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AmbientTreeStatePayload)) {
            UE_LOGW("event_feed: AmbientTreeState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AmbientTreeStatePayload));
            break;
        }
        net::AmbientTreeStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.landed > 1 || !std::isfinite(p.scale) || p.scale <= 0.f) {
            UE_LOGW("event_feed: AmbientTreeState malformed -- dropping");
            break;
        }
        ::coop::ambient_creature_sync::OnTreeState(p);
        break;
    }
    case net::ReliableKind::AmbientFixtureState: {
        // HOST->ALL.
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: AmbientFixtureState from slot %d -- dropping (host-authored)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AmbientFixtureStatePayload)) {
            UE_LOGW("event_feed: AmbientFixtureState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AmbientFixtureStatePayload));
            break;
        }
        net::AmbientFixtureStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.family > net::ambient_fixture::kCensusEnd || p.alive > 1 || p.hasNest > 1 || p.birthKey.len > sizeof(p.birthKey.data) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.pitch) || !std::isfinite(p.yaw) ||
            !std::isfinite(p.roll)) {
            UE_LOGW("event_feed: AmbientFixtureState malformed -- dropping");
            break;
        }
        ::coop::ambient_fixture_sync::OnState(p);
        break;
    }
    case net::ReliableKind::SusHoleIntent: {
        // CLIENT->HOST. The place's finiteness, the ring and the slot's range are the lane's to judge (as the spill case).
        if (session.role() != net::Role::Host || msg.payloadLen < sizeof(net::SusHoleIntentPayload)) {
            UE_LOGW("event_feed: a malformed or misdirected SusHoleIntent from slot %d -- dropping", msg.senderPeerSlot);
            break;
        }
        net::SusHoleIntentPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        ::coop::sus_hole_intent::OnIntent(session, msg.senderPeerSlot, p);
        break;
    }
    case net::ReliableKind::WalkerSeen: {
        // CLIENT->HOST. The flag, the key, the sender's standing as the walker's target and its distance are the module's to
        // judge, counted there.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: WalkerSeen from slot %d -- dropping (a client's, to the host)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::WalkerSeenPayload)) {
            UE_LOGW("event_feed: WalkerSeen payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WalkerSeenPayload));
            break;
        }
        net::WalkerSeenPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        ::coop::walker_gaze_host::OnSeen(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::WalkerState: {
        // HOST->ALL.
        if (session.role() == net::Role::Host) {
            UE_LOGW("event_feed: WalkerState from slot %d -- dropping (host-authored)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::WalkerStatePayload)) {
            UE_LOGW("event_feed: WalkerState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WalkerStatePayload));
            break;
        }
        net::WalkerStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.looking > 1 || p.key.len == 0 || p.key.len > sizeof(p.key.data) ||
            (p.target >= net::kMaxPeers && p.target != 0xFF)) {
            UE_LOGW("event_feed: WalkerState malformed -- dropping");
            break;
        }
        ::coop::walker_state_client::OnState(p);
        break;
    }
    case net::ReliableKind::GarbageBinState: {
        // HOST->ALL, a chunked blob. The payload's size, the side of the link, the body's every length and its version
        // are the module's to judge, counted there.
        ::coop::garbage_bin_sync::OnChunk(session, msg.payload, msg.payloadLen, msg.senderPeerSlot);
        break;
    }
    case net::ReliableKind::ContainerBreakRequest: {
        // CLIENT->HOST op 1, HOST->the requester op 2 or 3. The payload's exact size, the direction, the op and every
        // field are the module's to judge, counted there.
        ::coop::props::container_break::OnMessage(session, msg.senderPeerSlot, msg.payload, msg.payloadLen);
        break;
    }
    case net::ReliableKind::CarChargerState:
    case net::ReliableKind::CarChargerIntent: {
        // CarChargerState HOST->ALL, CarChargerIntent CLIENT->HOST (coop/interactables/car_charger_sync). Exact length,
        // counts bounded, every charge finite and within 0..100.
        const bool intent = msg.kind == net::ReliableKind::CarChargerIntent;
        const size_t want = intent ? sizeof(net::CarChargerIntentPayload) : sizeof(net::CarChargerStatePayload);
        if (intent != (session.role() == net::Role::Host) || msg.payloadLen != want) {
            UE_LOGW("event_feed: a car charger message the wrong way or the wrong length from slot %d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (intent) {
            net::CarChargerIntentPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            ::coop::car_charger_sync::OnIntent(msg.senderPeerSlot, p);
            break;
        }
        net::CarChargerStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.chargers > net::kCarChargerRowsMax || p.batteries > net::kCarBatteryRowsMax ||
            (p.chargers == 0 && p.batteries == 0))
            break;
        bool ok = true;
        for (int i = 0; i < p.batteries; ++i)
            ok = ok && std::isfinite(p.battery[i].charge) && p.battery[i].charge >= 0.f && p.battery[i].charge <= 100.f;
        if (!ok) {
            UE_LOGW("event_feed: CarChargerState with a charge non-finite or out of range -- dropping");
            break;
        }
        ::coop::car_charger_sync::OnState(p);
        break;
    }
    case net::ReliableKind::PryAsk: {
        // CLIENT->HOST: a client's crowbar pry of a plank, a radiator or a security TV, asked instead of run. Never
        // relayed. The host's judge (reach, the crowbar last held, class, still stuck) is coop::prop_pry_host.
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PryAsk dropped (role or sender slot %d)", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen != sizeof(net::PryAskPayload)) {
            UE_LOGW("event_feed: PryAsk from slot %d dropped (length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::PryAskPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || !std::isfinite(p.kickX) || !std::isfinite(p.kickY) ||
            !std::isfinite(p.kickZ)) {
            UE_LOGW("event_feed: PryAsk from slot %d dropped (key length %u or a kick that is not a number)",
                    msg.senderPeerSlot, static_cast<unsigned>(p.key.len));
            break;
        }
        coop::prop_pry_host::OnAsk(session, p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::PryState: {
        // HOST->CLIENT: a pried prop's state. Never relayed; a host has no one to take it from. The receiver is
        // coop::prop_pry_sync::OnState, which counts a key that resolves to no prop here.
        if (session.role() != net::Role::Client) {
            UE_LOGW("event_feed: PryState received on the host -- dropping");
            break;
        }
        if (msg.payloadLen != sizeof(net::PryStatePayload)) {
            UE_LOGW("event_feed: PryState dropped (length %zu)", static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::PryStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || (p.brokenMask & ~0x0Fu) != 0) {
            UE_LOGW("event_feed: PryState dropped (key length %u or a joint mask %02x past four joints)",
                    static_cast<unsigned>(p.key.len), static_cast<unsigned>(p.brokenMask));
            break;
        }
        coop::prop_pry_sync::OnState(p);
        break;
    }
    default:
        return false;  // not a driven-prop kind -> event_feed tries the next family
    }
    return true;  // a driven-prop kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
