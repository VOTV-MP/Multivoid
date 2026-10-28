// coop/props/container_act_request.h -- the client's half of an act on a world container: the act is asked of the
// host first (Ask), the host's reply is taken (a refusal shows a line, a grant is answered with the commit that ends
// the host's hold), and a commit the transport refused is sent again until it goes, so a host hold always ends.
// Gameplay layer (principle 7). Game thread.
//
// One act is pending at a time. An honest wait ends in milliseconds; the 10 s bound covers a transport that refused
// the reply. A request whose wait ran out is kept until the host answers it, and a grant that comes after the bound is
// answered with an abort; one older than the host's 30 s hold deadline is dropped. A refused commit is sent again
// every 250 ms at most.
//
// The take and put-record families have a replay: the player's verb is cancelled at its seat (a slot's pressButton;
// the screen's em_take or the container's playerUsedOn for a put) and asked of the host; on a grant the same verb runs
// again under the replay latch if it still names the record asked (a take's grant also younger than the host's hold
// less a margin), and the commit says whether and where the container changed. Any other family's grant is answered
// with an abort.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/container_act_wire.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::container_act_request {

void Install(coop::net::Session* session);   // also registers the take watch
void Tick();            // settles the take watch; sends the commits the transport refused again; the 10 s expiry of the
                        // open record
void OnDisconnect();    // the pending record, the replay latch, the expired list, the unsent commits, both seq counters,
                        // the line timers
void OnReply(const coop::net::ContainerActReplyPayload& p);

// True while a granted act's replay runs on this peer; the verb watch reads it. Game thread.
bool InReplay();

// Opens the one pending act and sends `r` with a fresh id written into it (ids skip 0). False when one is open, or
// the send failed ("The host could not be asked; try again."), or the request cannot be packed (a record past the byte
// cap: a line, nothing opened). Game thread.
bool Ask(coop::props::container_act_wire::Request r);

namespace internal {   // pure: the selftest drives it
enum class ReplyAction : uint8_t { Stale, Refused, BadReason, Granted };

// !pending or id != pendingId -> Stale; granted == 1 && reason == 0 -> Granted;
// granted == 0 && 1 <= reason < kContainerActReasons -> Refused; else BadReason.
ReplyAction DecideReply(bool pending, uint16_t pendingId, const coop::net::ContainerActReplyPayload& p);
const char* ReplyActionName(ReplyAction a);   // "stale" "refused" "bad-reason" "granted"

enum class TakeCheck : uint8_t { Replay, Abort };
// The replay presses only the slot that asked: its grant fresh (the host restores the record by itself at its hold
// deadline, so a replay past that makes the item twice), the slot live, its screen still showing the same container,
// the same id, and its record still hashing what the host granted. A failed read or an id past the end is
// hashMatches = false. The replay runs inline at the reply today, so a stale grant arises only from a replay held on
// the client (the drill's hold, or any future deferral).
TakeCheck CheckTake(bool grantFresh, bool slotLive, bool screenShowsContainer, bool sameId, bool hashMatches);
// The commit after the press. 0 (abort) only when the verb did not run, or it ran and took nothing (the count read
// back and did not fall); else 1 (done): a press that ran and whose count could not be read back counts as done, since
// an abort there would give back a record the player may hold.
uint8_t TakeCommitted(bool pressed, bool afterRead, size_t countBefore, size_t countAfter);

enum class PutCheck : uint8_t { Replay, Abort };
// R2 (the hotbar key) replays only the put that asked: the container live and still on the screen, no slot hovered,
// the take button enabled, the same selected index, and the record there still hashing what was asked.
PutCheck CheckPutR2(bool containerLive, bool screenShowsContainer, bool notHovered, bool takeEnabled, bool sameIndex,
                    bool hashMatches);
// R3 (hold and run) replays only for the actor that asked: the container live, that actor still the one in the hand,
// and its own record still hashing what was asked.
PutCheck CheckPutR3(bool containerLive, bool handIsAsked, bool hashMatches);
// R3's hash of a held actor's record, at the ask and at the replay: the record with the two fields that change while the
// player runs, its transform and floats[0][0] (the lifespan), zeroed, and with the fields of the held actor's class that
// tick by themselves zeroed too (`rows`, from PutExclusionRowsOf; 0 zeroes none). Any other change aborts the replay.
uint64_t PutHandHash(const ue_wrap::save_record::SaveRecord& record, uint32_t rows);
// The exclusion rows (a bit per row) whose class the held actor's class is or derives from. Game thread: it reads the
// class table. 0 for a null actor.
uint32_t PutExclusionRowsOf(void* heldActor);
// The row for a class name, or kNoExclusionRow: the pure half of the table, for the selftest.
inline constexpr size_t kNoExclusionRow = static_cast<size_t>(-1);
size_t PutExclusionRowOf(const wchar_t* className);
// Where a put's record went, from the container's record hashes before and after the put's verb: the index i when
// after is before with one record inserted at i (the first index where they differ, else the end). None when the count
// did not rise by one or the rest does not line up, so the verb stored nothing, or something else did.
std::optional<size_t> StoredIndex(const std::vector<uint64_t>& before, const std::vector<uint64_t>& after);
}  // namespace internal

namespace dev {   // the container act drills read it; OnDisconnect says it once; compiled always
uint32_t TakeDoneUnread();   // takes committed done although the press's count could not be read back
uint16_t PendingId();        // the open act's id, 0 when none is open
uint32_t StaleGrantAborts(); // grants aborted for their age instead of replayed, this session
uint8_t LastRefusal();       // the reason (ContainerActReason) of the open or last ask's refusal, 0 when none (an ask clears it)

// The last take commit this peer sent: the act's id, what it said (1 done, 0 abort), and how many take commits this
// session has sent (0: none yet). Reset at the session's end.
struct Take {
    uint16_t id = 0;
    uint8_t committed = 0;
    uint32_t count = 0;
};
Take LastTake();

// The last put act this peer asked, as the put seats saw it end. `route` is 2 (the hotbar key, R2) or 3 (hold and
// run, R3). `index` is the selected pane index at the ask (R2), then where the container stored the record once the
// put is done. `askHash` is the hash the host was asked and the replay compares: R3's with the held actor's pose and
// lifespan and its class's ticking fields zeroed (PutHandHash), R2's the record's own. `replayHash` is that same hash
// read again at the replay, 0 when the replay did not get as far as reading it. `askFloats` and `replayFloats` are the
// record's raw floats[0][1..3] (a food's temperature, ripeness and clock) at the ask and at the replay's own capture, NaN
// where the record has no such index or no capture was made. Refused is the host's answer to the ask.
// Reset at the session's end.
enum class PutOutcome : uint8_t { None, Asked, Done, Aborted, Refused };
struct Put {
    uint16_t id = 0;
    uint8_t route = 0;
    PutOutcome outcome = PutOutcome::None;
    uint16_t index = 0;
    uint64_t askHash = 0;
    uint64_t replayHash = 0;
    float askFloats[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::quiet_NaN()};
    float replayFloats[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::quiet_NaN()};
};
Put LastPut();
const char* PutOutcomeName(PutOutcome o);   // "none" "asked" "done" "aborted" "refused"

// Inert unless called; all reset at the session's end.
void HoldReplay(bool on);   // while on, a take's or a put's grant parks its replay (the record stays open, a take's
                            // grant time kept)
bool ReplayParked();        // a grant is parked
void ReleaseReplay();       // runs the parked replay now, through the same checks as an unparked one
void DoubleAbort(bool on);  // while on, a take's abort commit is sent twice
void DoubleDone(bool on);   // while on, a put's done commit is sent twice

// The put drill's reds (CLIENT). NoHoverGuard: R2's replay no longer requires that no slot is hovered. NoHandTable: the
// hold-and-run hash leaves out no class's ticking fields (PutExclusionRowsOf answers none).
void NoHoverGuard(bool on);
void NoHandTable(bool on);

// Acts this session by how the client ended them: a commit sent done, a commit sent aborted, a reply that refused. The
// census drill judges each route by the delta of these.
uint32_t ActsDone();
uint32_t ActsAborted();
uint32_t ActsRefused();
}  // namespace dev

}  // namespace coop::props::container_act_request
