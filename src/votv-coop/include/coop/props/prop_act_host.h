// coop/props/prop_act_host.h -- the host's half of a client's take or use of a shared prop: a request is judged (the
// prop live, its family, nobody else holding or reserving it, the reach), a grant reserves the prop for the sender,
// and the sender's commit makes the host destroy the prop itself and tell every other peer. Gameplay layer
// (principle 7). Game thread unless marked.
//
// The queue's Wait is the mid-join answer: a request before the sender's world is ready, before its body has a pose
// or while it holds a reservation stays at the head of its queue, and every one of those ends by the client's own
// event. The reservation has no timer on the host: the commit, the abort or the sender's departure ends it.

#pragma once

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"

#include <cstdint>
#include <optional>

namespace coop::net { class Session; }

namespace coop::prop_act_host {

void Install(coop::net::Session* session);
void Tick();                                      // serves every slot's queue; flushes the refusal tally
void OnDisconnect();                              // the session ended: tallies, refusals and reservations cleared
void OnPeerGone(uint8_t slot);                    // the DisconnectSlot fan-out; host only, self-gated
void OnRequest(const coop::net::PropActRequestPayload& p, uint8_t senderSlot);
void OnCommit(const coop::net::PropActCommitPayload& p, uint8_t senderSlot);
void NoteBadSender();                             // the dispatcher saw a sender outside 1..kMaxPeers-1
void NoteShortPayload(uint8_t senderSlot);        // the dispatcher saw a short payload

namespace internal {   // pure, no engine call: prop_act_selftest drives these (NOT the game thread)
enum class ActVerdict : uint8_t { Wait, Gone, WrongFamily, Held, Reach, Grant };
struct ActInputs {
    bool worldReady = false, holdsReservation = false, live = false, familyFits = false, heldByOther = false,
         hasBody = false;
    coop::element::IntentOutcome reach = coop::element::IntentOutcome::NoBody;
};
ActVerdict Decide(const ActInputs& in);
std::optional<coop::net::PropActReplyPayload> ReplyFor(ActVerdict v, uint16_t id);
enum class CommitStep : uint8_t { BadCommit, Abort, TargetGone, Destroy };
CommitStep DecideCommit(bool matched, uint8_t committed, bool liveAndSame);
enum class AfterStep : uint8_t { DestroyFailed, FanOut };
AfterStep AfterDestroy(bool destroyed);
const char* VerdictName(ActVerdict v);   // "wait" "gone" "wrong-family" "held" "reach" "grant"
const char* CommitName(CommitStep s);    // "bad" "abort" "target-gone" "destroy"
}  // namespace internal

enum class Tally : uint8_t { TargetGone, BadCommit, HeldAtDisconnect, DestroyFailed, QueueFullUnready, BadSender,
                             GrantUnsent, kCount };
enum class WaitCause : uint8_t { NotReady, Reservation, NoBody, kCount };

}  // namespace coop::prop_act_host
