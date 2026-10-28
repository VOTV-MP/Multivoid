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
// The Take and PutRecord families are served here; PutProp is answered WrongFamily until its package arrives.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_reach.h"

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
// PutDoneLost the timely puts a re-check refused at the insert, OwedLost the owed puts given up.
enum class Tally : uint8_t { TargetGone, BadCommit, GrantUnsent, QueueFullUnready, BadSender, ShortPayload,
                             Malformed, UnknownFamilyUnready, RestoreOwed, WaitExpired, ReservationLapsed, LateDone,
                             LateDoneLost, PutNoName, PutClassUnresolved, PutNotProp, PutFiltered, PutIntoMailbox,
                             PutRowRefuses, PutUnknownName, PutDoneLost, OwedLost, kCount };
const char* TallyName(Tally t);   // "target_gone" ... "owed_lost"; "" past kCount

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
// -> Refused; countAtCap, bytesOver -> NoSpace; then unless infinite: !volumeRead -> Wait, !volumeKnown -> Refused,
// currVol + volume > maxVol -> NoSpace; else Grant. `refusal`, when given, gets the tally term of a Refused
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

// The client's word is bounded where it is stored (MTA, CGame.cpp:2731-2771): a done's record is no larger than the
// one asked plus the slack the container's own flag needs, and a put's name is at most kMaxPutNameChars.
bool DoneSizeOk(size_t askedBytes, const ue_wrap::save_record::SaveRecord& committed);
bool PutNameOk(const ue_wrap::save_record::SaveRecord& r);

// What a put's Wait waits on, for the line that ends the wait: the first term whose read did not come.
const char* PutWaitCause(const PutInputs& in);

// The index when its hash matches, else the first record with the hash; none: Gone.
std::optional<size_t> FindRecord(const std::vector<uint64_t>& hashes, uint16_t index, uint64_t hash);

// The least gap between two truth sends to one slot for one container.
inline constexpr uint64_t kTruthEveryMs = 2000;
bool TruthDue(uint64_t lastSentMs, uint64_t nowMs);

// The least gap between two truth sends to one slot across all containers; `lastAnyMs` 0 is none sent yet.
inline constexpr uint64_t kTruthSlotEveryMs = 500;
bool SlotTruthDue(uint64_t lastAnyMs, uint64_t nowMs);

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
// taken from and one slot per peer; Verdicts counts the grants sent or queued and the Gone replies, not the "hold not
// opened" Gone.
namespace dev {

// While on, a Granted reply is queued instead of sent: the escrow is made, its deadline runs, the lane publishes as usual.
void HoldReplies(bool on);
// Sends the queued replies in order; one that cannot be sent takes the unsent-grant path (the record goes back).
void ReleaseReplies();

// The red: a grant opens its escrow with the record found but does not remove it from the container, so a second
// taker finds it still there.
void NoEscrow(bool on);

// The RecordHash of the record held in escrow on `eid` (the neutered copy); false when none.
bool FirstEscrow(uint32_t eid, uint64_t& hash);

// The take verdicts this session answered for `eid`: grants sent (or queued) and Gone replies.
void Verdicts(uint32_t eid, uint32_t& grants, uint32_t& gones);
}  // namespace dev

}  // namespace coop::props::container_act_host
