// coop/dispatch/event_dispatch_kerfur_command.cpp -- a client's radial command for a kerfur and the host's answer:
// KerfurCommand (client to host, handed to coop::kerfur_command) and KerfurCommandAnswer (host to the requester,
// handed to coop::kerfur_command_answer). Each case validates at the trust boundary. The switch below is the
// family's membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h: returns
// true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_command_answer.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleKerfurCommandEvent(net::Session& session,
                              const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::KerfurCommand: {
        // client->host kerfur radial-menu command (follow/idle/patrol/...), a chunked blob. Host-authoritative; the
        // handler assembles it, waits for the sender's body, validates the element + kerfur class + the BP guards,
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
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            coop::kerfur_command::NoteRefused(senderSlot, "payload too short");
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        coop::kerfur_command::OnCommandChunk(chunk, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurCommandAnswer: {
        // HOST->CLIENT: the host's refusal of, or wait on, a command this client sent. Never relayed.
        // coop::kerfur_command_answer.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::KerfurCommandAnswerPayload)) {
            UE_LOGW("event_feed: KerfurCommandAnswer dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::KerfurCommandAnswerPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfur_command_answer::OnAnswer(p);
        break;
    }
    default:
        return false;  // not a kerfur-command kind -> event_feed tries the next family
    }
    return true;  // a kerfur-command kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
