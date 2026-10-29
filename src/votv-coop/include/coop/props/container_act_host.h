// coop/props/container_act_host.h -- the host's half of a client's act on a world container: a request is judged
// (the sender's world, the family, the holds, the reach, the container, the record's address or its own terms), and
// the sender's commit ends the hold. A take's grant puts the record into escrow: done drops it, abort restores it. A
// put's grant reserves room and writes nothing: done inserts the record the client's container stored, abort releases.
// Gameplay layer (principle 7). Game thread unless marked.
//
// Every hold ends: by the commit, by its 30 s deadline, by the sender's departure or by the session's end. A write
// that cannot happen now is owed and retried at 1 Hz. A reservation that lapses leaves a tombstone, so a late done is
// still inserted, and slows its sender. The queue's Wait is the mid-join answer: a request before the sender's world
// is ready, before its body has a pose, or while a hold stands in its way stays at the head of its queue until
// kWaitBoundMs after it was queued; then it is answered Gone.
//
// The Take and PutRecord families are served here. A world prop that a client drives into a container's box asks nothing:
// the host judges it at its own overlap (namespace overlap, container_act_host_overlap.cpp).

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_reach.h"

#include "ue_wrap/core/script_gate.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::container_act_host {

void Install(coop::net::Session* session);
void Tick();                                      // serves every slot's queue; the 1 Hz holds and owed restores
void OnDisconnect();                              // the session ended or the last client left: holds restored
void OnPeerGone(uint8_t slot);                    // the DisconnectSlot fan-out; host only, self-gated
void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void NoteBadSender();                             // the dispatcher saw a sender outside 1..kMaxPeers-1
void NoteShortPayload(uint8_t senderSlot);        // the dispatcher saw a short payload
void RunSelftest();                               // the pure decisions, once per start; NOT the game thread

// The host's dev counters (TallyOf). The Put* terms are the put's refusals, each by name; ReservationLapsed counts
// reservations that ended by their deadline, LateDone the late puts inserted, LateDoneLost the late puts a cap refused,
// PutDoneLost the timely puts a re-check refused at the insert, OwedLost the owed puts given up, TakeArmed the final
// takes after which the host called the container's own dingus (the game's arming of a container a record was taken from).
// The PutOverlap* terms are the overlap put's: Filed (the prop is gone after the body), Unarmed (the box is not armed),
// Reach (a held or claimed prop is out of its driver's reach), Unread (a read the judge needs did not come), Held (a
// client holds the container: the put waits), Fit (the put terms refuse it), Gate (the body's silent gates would swallow
// it), Gap (the body ran and the prop lived: a term the judge did not see), WaitDropped (a wait fell out of a full table),
// Left (a waiting put ended with no verdict: its prop or container gone, the prop out of the box or no longer a put) and LatchEvicted (a refusal latch fell out of a full table).
enum class Tally : uint8_t { TargetGone, BadCommit, GrantUnsent, QueueFullUnready, BadSender, ShortPayload,
                             Malformed, UnknownFamilyUnready, RestoreOwed, WaitExpired, ReservationLapsed, LateDone,
                             LateDoneLost, PutNoName, PutClassUnresolved, PutNotProp, PutFiltered, PutIntoMailbox,
                             PutRowRefuses, PutUnknownName, PutDoneLost, OwedLost, TakeArmed, PutOverlapFiled,
                             PutOverlapUnarmed, PutOverlapReach, PutOverlapUnread, PutOverlapHeld, PutOverlapFit,
                             PutOverlapGate, PutOverlapGap, PutOverlapWaitDropped, PutOverlapLeft, PutOverlapLatchEvicted, kCount };
const char* TallyName(Tally t);   // "target_gone" ... "put_overlap_latch_evicted"; "" past kCount

// The host's judge of a world prop a client drives into a container's box (container_act_host_overlap.cpp): a PRE and a POST
// on prop_container_C::putObjectIn_overlap, registered by container_act_put's watch table on both roles and acting on the
// host alone. OnContainerFree: a hold on container `eid` ended, so the puts that waited on it are judged again.
namespace overlap {
void Install(coop::net::Session* session);
ue_wrap::script_gate::Verdict Pre(const ue_wrap::script_gate::Call& c);
void Post(const ue_wrap::script_gate::Call& c);
void OnContainerFree(uint32_t eid);
void Tick();    // the waiting puts whose edge came are judged again; those whose time ran out are refused as unread
void OnBound(void* actor);                 // an actor was bound to an element id (RegisterPropMirror): a put waiting for its bind
void OnContainerChanged(uint32_t eid);     // a container was marked dirty (its records or arming changed): its latches clear
void Reset();   // the session's end: the waiting puts and the pending body are dropped
}  // namespace overlap

namespace internal {   // pure, no engine call: the selftest drives these (NOT the game thread)

enum class Verdict : uint8_t { Wait, Gone, Reach, WrongFamily, Grant, Locked, NoSpace, Refused, Lapsed };
enum class Container : uint8_t { Gone, NotReady, Ready };

// Decide's inputs in the order Decide reads them; ReadInputs fills them in that order and stops at the first that
// decides, so a request that waits reads only what the wait needs.
struct Inputs {
    bool worldReady = false, served = false, holdsWait = false;
    coop::props::container_reach::Reach reach = coop::props::container_reach::Reach::NoBody;
    Container container = Container::Gone;
    bool found = false;
};

// Order: !worldReady -> Wait; !served -> WrongFamily; holdsWait -> Wait; reach NoBody -> Wait; container Gone -> Gone;
// container NotReady -> Wait; reach Unreachable -> Reach; !found -> Gone; else Grant.
Verdict Decide(const Inputs& in);

// A put's inputs: `base` is the part every family reads (the world, the holds, the reach, the container), then what
// the put's own terms read, each flag in the order DecidePut reads it. The defaults pass every term.
struct PutInputs {
    Inputs base;
    bool termsRead = true;                    // the container's PutTerms read; false: a transient read, so Wait
    bool locked = false;
    bool slowed = false;                      // the sender let a reservation lapse and is slowed
    bool nameOk = true;                       // the record carries names[0][0]
    bool classResolved = true, propClass = true;
    bool filterRead = true, filterHolds = false;
    bool mailboxContainer = false;            // the container is a mailbox and the record a container
    bool rowRead = true, rowRefuses = false;  // the list_props row is readable; a row says not collectable or heavy
    bool countAtCap = false, bytesOver = false;
    bool infinite = false;
    bool volumeRead = true, volumeKnown = true;
    float maxVol = 0.f, currVol = 0.f, volume = 0.f;
};

// Order: !worldReady -> Wait; holdsWait -> Wait; reach NoBody -> Wait; container Gone -> Gone; NotReady -> Wait;
// Unreachable -> Reach; !termsRead -> Wait; locked -> Locked; slowed -> Lapsed; !nameOk, !classResolved, !propClass
// -> Refused; !filterRead -> Wait; filterHolds -> Refused; mailboxContainer -> Refused; !rowRead -> Wait; rowRefuses
// -> Refused; !volumeRead -> Wait; !volumeKnown -> Refused (the name is not in the game's table, whatever the
// container); countAtCap, bytesOver -> NoSpace; unless infinite: currVol + volume > maxVol -> NoSpace; else Grant.
// `refusal`, when given, gets the tally term of a Refused
// verdict, and Tally::kCount for any other.
Verdict DecidePut(const PutInputs& in, Tally* refusal = nullptr);

enum class CommitStep : uint8_t { BadCommit, Done, Abort };

// The commit names this hold: `h` is Match(slot, id), non-null and h->eid equals the commit's eid. An Escrow's
// commit carries no record. A Reservation's commit carries a record exactly when it is done (`committed` == 1).
bool CommitMatches(const coop::props::container_act_state::Hold* h, uint32_t commitEid, bool hasRecord,
                   uint8_t committed);
CommitStep DecideCommit(bool matched, uint8_t committed);

// A put's done names what was asked: the stored record's class and names[0][0] equal the asked record's, and both
// carry a name. The rest of the record is the container's own (the fridge flag, the rest of its state).
bool StoredMatches(const ue_wrap::save_record::SaveRecord& asked, const ue_wrap::save_record::SaveRecord& stored);

// The client's word is bounded where it is stored (MTA, CGame.cpp:2731-2736, 2743-2753): a done's record is no larger than the
// one asked plus the slack the container's own flag needs, and a put's name is at most kMaxPutNameChars.
bool DoneSizeOk(size_t askedBytes, const ue_wrap::save_record::SaveRecord& committed);
bool PutNameOk(const ue_wrap::save_record::SaveRecord& r);

// What a put's Wait waits on, for the line that ends the wait: the first term whose read did not come.
const char* PutWaitCause(const PutInputs& in);

// The index when its hash matches, else the first record with the hash; none: Gone.
std::optional<size_t> FindRecord(const std::vector<uint64_t>& hashes, uint16_t index, uint64_t hash);

// A head that waited this long since it was queued is answered Gone. Above the client's own 10 s reply wait, so an
// honest client has already moved on.
inline constexpr uint64_t kWaitBoundMs = 15000;
bool WaitExpired(uint64_t queuedMs, uint64_t nowMs);

const char* VerdictName(Verdict v);   // "wait" "gone" "reach" "wrong-family" "grant" "locked" "no-space" "refused" "lapsed"
const char* CommitName(CommitStep s); // "bad" "done" "abort"

}  // namespace internal

uint32_t TallyOf(Tally t);

// Called only from src/coop/dev/ (the container act drill); each knob is inert unless called, all reset at the session
// end. The two counters FirstEscrow and Verdicts read are written in every host session, a map entry per container
// taken from or put into and one slot per peer; Verdicts counts every family's grants sent or queued and Gone replies,
// not the "hold not opened" Gone.
namespace dev {

// While on, a Granted reply is queued instead of sent: the escrow is made, its deadline runs, the lane publishes as usual.
void HoldReplies(bool on);
// Sends the queued replies in order; one that cannot be sent takes the unsent-grant path (the record goes back).
void ReleaseReplies();

// The red: a grant opens its escrow with the record found but does not remove it from the container, so a second
// taker finds it still there.
void NoEscrow(bool on);

// The put drill's knobs. NoReservation: a put's grant opens no hold. NoLapse: a sender is never slowed (its next put is
// not answered Lapsed). InsertNotReady: mode 1 answers the insert of a put's record NotReady, so the put is owed and
// retried, then given up after kMaxOwedTries; mode 2 answers an escrow's restore so too (which is retried for good);
// 0 puts it back.
void NoReservation(bool on);
void NoLapse(bool on);
void InsertNotReady(int mode);

// The host's judgements of a put head, counted while on: for a slot, the id of its latest put head, how many times the
// host judged it (the early return of a head that waits is not one), and the first and last of those, in NetClock ms.
struct PutJudged {
    uint16_t id = 0;
    uint32_t n = 0;
    uint64_t firstMs = 0, lastMs = 0;
};
void CountPutJudgements(bool on);
PutJudged LastPutJudged(uint8_t slot);

// While CountPutJudgements is on, the host's grants (sent or queued) and put commits inserted, in the order it did them,
// a ring of the last 16. OrderSeq is the rising number (from 1) of the oldest such event of `slot` on `eid` still in the
// ring, a grant when `grant` is true and an inserted commit otherwise; 0 when there is none.
uint32_t OrderSeq(uint32_t eid, uint8_t slot, bool grant);

// The RecordHash of the record held in escrow on `eid` (the neutered copy); false when none.
bool FirstEscrow(uint32_t eid, uint64_t& hash);

// The verdicts this session answered for `eid`, takes and puts together: grants sent (or queued) and Gone replies.
void Verdicts(uint32_t eid, uint32_t& grants, uint32_t& gones);
}  // namespace dev

}  // namespace coop::props::container_act_host
