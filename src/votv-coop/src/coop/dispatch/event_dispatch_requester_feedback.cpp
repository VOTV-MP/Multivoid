// coop/dispatch/event_dispatch_requester_feedback.cpp -- what a verb the host ran for a client showed on the host:
// RequesterFeedback (host to client, handed to coop::requester_feedback). The case validates at the trust boundary.
// The switch below is the family's membership declaration; nothing re-lists it. Family contract per
// coop/dispatch/event_dispatch.h: returns true iff msg.kind is in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/requester_feedback/requester_feedback.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleRequesterFeedbackEvent(net::Session& session,
                                  const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::RequesterFeedback: {
        // HOST->CLIENT: a hint, gloss or 2D sound a verb the host ran for this client showed on the host, for this
        // machine to show. Never relayed. coop::requester_feedback.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::RequesterFeedbackPayload)) {
            UE_LOGW("event_feed: RequesterFeedback dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::RequesterFeedbackPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::requester_feedback::OnFeedback(p);
        break;
    }
    default:
        return false;  // not a requester-feedback kind -> event_feed tries the next family
    }
    return true;  // a requester-feedback kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
