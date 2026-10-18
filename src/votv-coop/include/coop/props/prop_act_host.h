// coop/props/prop_act_host.h -- the host's half of a client's take or use of a shared prop: a request is judged (the
// prop live, nobody else holding or reserving it, the reach, and last the act itself: the game's own conditions for
// the verb the request names, read on the host's copy), a grant reserves the prop for the sender, and the sender's
// commit makes the host destroy the prop itself and tell every other peer. Gameplay layer (principle 7). Game thread
// unless marked.
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
void RunSelftest();                               // the pure decisions, once per start; NOT the game thread

// The host's dev counters (TallyOf). The Fit* tallies name the term of an act's conditions that refused it; the reply
// to the sender stays WrongFamily.
enum class Tally : uint8_t { TargetGone, BadCommit, HeldAtDisconnect, DestroyFailed, QueueFullUnready, BadSender,
                             GrantUnsent, FitUnread, FitBadFamily, FitTrash, FitNotProp, FitDenied, FitNoUseRow,
                             FitCollectRefused, FitPickupRefused, FitIgnoreSave, FitChild, FitPhysics, FitCanCollect,
                             FitCanHold, FitHeavy, FitMeshRoot, FitVolume, FitSaveClass, FitEquipRow, FitNotEdible,
                             kCount };
const char* TallyName(Tally t);   // "target_gone" ... "fit_not_edible"; "" past kCount

namespace internal {   // pure, no engine call: prop_act_selftest drives these (NOT the game thread)
enum class ActVerdict : uint8_t { Wait, Gone, WrongFamily, Held, Reach, Grant };
// Decide's inputs in the order Decide reads them. familyFits is read last: a request is refused for its act only
// once everything cheaper (the body, the prop, the hold, the reach) has passed.
struct ActInputs {
    bool worldReady = false, holdsReservation = false, hasBody = false, live = false, heldByOther = false;
    coop::element::IntentOutcome reach = coop::element::IntentOutcome::NoBody;
    bool familyFits = false;
};
ActVerdict Decide(const ActInputs& in);

// The facts one act is judged on. `read` is false when a fact could not be read, and then nothing fits.
struct ActTraits {
    bool read, collectRefused, pickupRefused, ignoreSave, child, frozen, simulating, driven, canCollect, canHold,
         heavy, meshRoot, volumeFits, saveClass, equipRow, offered, foodBody, notEdible;
};
bool PocketFits(const ActTraits& t);   // putObjectInventory2 and addObject
bool HoldFits(const ActTraits& t);     // Hold Object and updateHold
bool UseFits(const ActTraits& t);      // the use verbs the lane carries
bool KnownFamily(uint8_t family);      // 1..3
// The first term of the verb's fit that is false, in the order the fit reads them; none when the act fits.
std::optional<Tally> FirstRefusal(uint8_t family, const ActTraits& t);
std::optional<coop::net::PropActReplyPayload> ReplyFor(ActVerdict v, uint16_t id);
enum class CommitStep : uint8_t { BadCommit, Abort, TargetGone, Destroy };
CommitStep DecideCommit(bool matched, uint8_t committed, bool liveAndSame);
enum class AfterStep : uint8_t { DestroyFailed, FanOut };
AfterStep AfterDestroy(bool destroyed);
const char* VerdictName(ActVerdict v);   // "wait" "gone" "wrong-family" "held" "reach" "grant"
const char* CommitName(CommitStep s);    // "bad" "abort" "target-gone" "destroy"
}  // namespace internal

enum class WaitCause : uint8_t { NotReady, Reservation, NoBody, kCount };

// The counters the drills judge by. A slot outside 1..kMaxPeers-1 reads 0.
uint32_t AnsweredFor(int slot, coop::net::PropActReason reason);   // replies sent per reason (Granted = grants)
uint32_t TallyOf(Tally t);
uint32_t WaitsOf(int slot, WaitCause c);                          // served Waits by cause (a counter, never a log)
uint32_t MaxDepthOf(int slot);                                    // the deepest the slot's queue has been this session
uint32_t AnsweredTotal(int slot);                                 // the sum of AnsweredFor over every reason
uint32_t CommitsOf(int slot, internal::CommitStep s);             // commits taken per step

namespace dev {   // called only from src/coop/dev/ (the act drills); compiled always, inert unless called
// Would the judge accept `family` on `actor`? FamilyFits' path with no tally bumped. `*first` is the term that
// refuses, Tally::kCount when the act fits. Game thread.
bool CensusFits(void* actor, uint8_t family, Tally* first);
}  // namespace dev

}  // namespace coop::prop_act_host
