// coop/dispatch/event_dispatch_container_act.cpp -- a client's act on a world container: ContainerActRequest and
// ContainerActCommit (client to host, chunked blobs, handed to coop::props::container_act_host) and ContainerActReply
// (host to client, the answer, handed to coop::props::container_act_request). Each case validates at the trust
// boundary. The switch below is the family's membership declaration; nothing re-lists it. Family contract per
// coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/props/container_act_host.h"     // the host's judge of a request, and the commit that ends a hold
#include "coop/props/container_act_request.h"  // the client's reply receiver

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleContainerActEvent(net::Session& session,
                             const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::ContainerActRequest: {
        // CLIENT->HOST: a client asks to take a record out of a world container, or to put one in. A chunked blob the
        // host's lane assembles, parses and judges (coop::props::container_act_host). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ContainerActRequest received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot = (msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers)
                                       ? static_cast<uint8_t>(msg.senderPeerSlot)
                                       : static_cast<uint8_t>(0xFF);
        // The refusal is the lane's to say, in its tally: a sender cannot make the router's log the cost.
        if (senderSlot == 0xFF) {
            coop::props::container_act_host::NoteBadSender();
            break;
        }
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            coop::props::container_act_host::NoteShortPayload(senderSlot);
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        coop::props::container_act_host::OnRequestChunk(chunk, senderSlot);
        break;
    }
    case net::ReliableKind::ContainerActCommit: {
        // CLIENT->HOST: the client's verb on a granted record ran or did not; the commit ends the host's hold
        // (coop::props::container_act_host). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ContainerActCommit received on a client -- dropping");
            break;
        }
        const uint8_t senderSlot = (msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers)
                                       ? static_cast<uint8_t>(msg.senderPeerSlot)
                                       : static_cast<uint8_t>(0xFF);
        if (senderSlot == 0xFF) {
            coop::props::container_act_host::NoteBadSender();
            break;
        }
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            coop::props::container_act_host::NoteShortPayload(senderSlot);
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        coop::props::container_act_host::OnCommitChunk(chunk, senderSlot);
        break;
    }
    case net::ReliableKind::ContainerActReply: {
        // HOST->CLIENT: the host's answer to this client's container act: a grant is answered with the commit that
        // ends the host's hold, a refusal shows a line (coop::props::container_act_request). Never relayed.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::ContainerActReplyPayload)) {
            UE_LOGW("event_feed: ContainerActReply dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::ContainerActReplyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::props::container_act_request::OnReply(p);
        break;
    }
    default:
        return false;  // not a container-act kind -> event_feed tries the next family
    }
    return true;  // a container-act kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
