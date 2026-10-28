// coop/dispatch/event_dispatch_prop_drive.cpp -- the driven prop's reliable kinds: PropDriveEnd (the
// close of a prop's stream, the host's to all or a syncing client's to the host) and the prop
// syncer's pair, PropSyncClaim (client to host) and PropSyncAnswer (host to one client), and the developer
// filler DevLoadFiller (host to one client), DevParityReport (a joiner's pose report to the host) and
// DevContainerParity (a joiner's container content report to the host). Each case
// validates at the trust boundary and hands off to its module. The switch below is the family's
// membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h:
// returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/dev/container_parity.h"     // the container join drill's report receiver
#include "coop/dev/prop_syncer_drill.h"    // the drill's filler receiver
#include "coop/dev/pile_defer_report.h"    // the pile deferral drill's report receiver
#include "coop/element/registry.h"
#include "coop/props/prop_drive_stream.h"  // the driven-prop end edge
#include "coop/props/prop_syncer.h"        // the client's answer receiver
#include "coop/props/prop_syncer_host.h"   // the host's claim judge and a client's End

#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstring>

namespace coop::event_feed {

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
    default:
        return false;  // not a driven-prop kind -> event_feed tries the next family
    }
    return true;  // a driven-prop kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
