// coop/dispatch/event_dispatch_kerfur_command.cpp -- a client's radial command for a kerfur and the host's answer:
// KerfurCommand (client to host, handed to coop::kerfur_command) and KerfurCommandAnswer (host to the requester,
// handed to coop::kerfur_command_answer), and an Omega's outfit and pat: KerfurDressState (host to every client) and
// KerfurDressIntent (a client's pat, to the host), both handed to coop::kerfur_dress_sync. Each case validates at the trust boundary. The switch below is the
// family's membership declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h: returns
// true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_command_answer.h"
#include "coop/creatures/kerfur_dress_sync.h"

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
    case net::ReliableKind::KerfurDressState: {
        // HOST->CLIENT: an Omega's whole outfit, and a pat to show. Never relayed. coop::kerfur_dress_sync. The length is
        // exactly the word's 224 bytes, then WellFormed walks `names` (count rows of 1..26 printable ASCII, zeros after).
        net::KerfurDressPayload p{};
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 || msg.payloadLen != sizeof(p)) {
            UE_LOGW("event_feed: KerfurDressState dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        std::memcpy(&p, msg.payload, sizeof(p));
        if (!coop::kerfur_dress_sync::internal::WellFormed(p)) {
            UE_LOGW("event_feed: KerfurDressState malformed (count %u, flags %u) -- dropping", static_cast<unsigned>(p.count),
                    static_cast<unsigned>(p.flags));
            break;
        }
        coop::kerfur_dress_sync::OnState(p);
        break;
    }
    case net::ReliableKind::KerfurDressIntent: {
        // CLIENT->HOST: this client's player patted an Omega. Never relayed. coop::kerfur_dress_sync.
        net::KerfurDressPatPayload p{};
        if (session.role() != net::Role::Host || msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers ||
            msg.payloadLen != sizeof(p)) {
            UE_LOGW("event_feed: KerfurDressIntent dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            coop::kerfur_dress_sync::NoteIntentDropped();
            break;
        }
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfur_dress_sync::OnPatAsk(msg.senderPeerSlot, p);
        break;
    }
    default:
        return false;  // not a kerfur-command kind -> event_feed tries the next family
    }
    return true;  // a kerfur-command kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
