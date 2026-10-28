// coop/props/container_act_host.h -- the host's half of a client's act on a world container: a request is judged
// (the sender's world, the family, the holds, the reach, the container, the record's address), a grant takes the
// record out of the container into escrow, and the sender's commit ends the hold: done drops the record (the client
// has it), abort puts it back. Gameplay layer (principle 7). Game thread unless marked.
//
// Every hold ends: by the commit, by its 30 s deadline, by the sender's departure or by the session's end, and each
// end restores the record. A restore that cannot write now is owed and retried at 1 Hz, never dropped while the
// session runs. The queue's Wait is the mid-join answer: a request before the sender's world is ready, before its
// body has a pose, or while a hold stands in its way stays at the head of its queue, until kWaitBoundMs after it
// was queued; then it is answered Gone.
//
// The Take family is served here; the put families are answered WrongFamily until their packages arrive.

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

// The host's dev counters (TallyOf).
enum class Tally : uint8_t { TargetGone, BadCommit, GrantUnsent, QueueFullUnready, BadSender, ShortPayload,
                             Malformed, UnknownFamilyUnready, RestoreOwed, WaitExpired, kCount };
const char* TallyName(Tally t);   // "target_gone" ... "wait_expired"; "" past kCount

namespace internal {   // pure, no engine call: the selftest drives these (NOT the game thread)

enum class Verdict : uint8_t { Wait, Gone, Reach, WrongFamily, Grant };
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

enum class CommitStep : uint8_t { BadCommit, Done, Abort };

// The commit names this hold: `h` is Match(slot, id), non-null, an Escrow, h->eid equals the commit's eid, and the
// commit carries no record. A Reservation's rule is the put package's, decided by the hold's kind, never the
// commit's; here no Reservation is ever opened, so a Reservation never matches.
bool CommitMatches(const coop::props::container_act_state::Hold* h, uint32_t commitEid, bool hasRecord);
CommitStep DecideCommit(bool matched, uint8_t committed);

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

const char* VerdictName(Verdict v);   // "wait" "gone" "reach" "wrong-family" "grant"
const char* CommitName(CommitStep s); // "bad" "done" "abort"

}  // namespace internal

uint32_t TallyOf(Tally t);

}  // namespace coop::props::container_act_host
