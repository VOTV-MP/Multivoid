// coop/props/prop_act_request.h -- the client's half of a take or use of a shared prop: a watch on each verb cancels
// a client's call on a shared prop and asks the host (a use row asks only for a call the client's copy predicts will
// destroy the prop), the host's reply is taken (a refusal shows a line, a grant replays the verb under a latch and
// sends the commit) and the destroy seam is told which destroy is the replay's. The same watches cancel the host's own
// take or use of a prop a client has reserved. Gameplay layer (principle 7). Game thread.
//
// One act is pending at a time. An honest wait ends in milliseconds, since every host Wait ends on the client's own
// event; the 10 s bound covers a transport that refused the reply. Every request whose wait ran out is kept until the
// host answers it, and a grant that comes after the bound is answered with an abort, so the host's reservation never
// outlives the client's wait.

#pragma once

#include "coop/net/protocol.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"   // FName
#include "ue_wrap/core/types.h"        // FVector

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::prop_act_request {

void Install(coop::net::Session* session);      // registers the six watches on a take or use, on either role
void Tick();                                     // settles the watches; sends the commits the transport refused again;
                                                 // the 10 s expiry of an open record; a parked replay (dev)
void OnDisconnect();                             // the pending record, the next id, the unsent commits, the latch, the
                                                 // expired requests, the last stale reply and the counters cleared
void OnReply(const coop::net::PropActReplyPayload& p);

// The destroy seam asks, before its keyed-interactable gate: is `actor` the target of the replay running now? Sets the
// latch's destroyed flag on a match.
bool NoteSeamDestroy(void* actor);

namespace internal {   // pure: prop_act_selftest drives it
enum class ReplyAction : uint8_t { Stale, Refused, BadReason, Granted };
ReplyAction DecideReply(bool pending, uint16_t pendingId, const coop::net::PropActReplyPayload& p);
const char* ReplyActionName(ReplyAction a);   // "stale" "refused" "bad-reason" "granted"
}  // namespace internal

// The row of the verb a record replays.
enum Row : uint8_t { kPocket = 1, kHold, kFood, kMre, kPills, kSoap };

// The pending record: the target's element id and the call's arguments, as the replay needs them.
struct Pending {
    bool open = false;
    uint16_t id = 0;
    uint32_t eid = 0;
    uint8_t row = 0;                               // a Row
    ue_wrap::CachedObjRef target;
    uint64_t sentMs = 0;
    bool noNotify = false;                         // kPocket
    ue_wrap::CachedObjRef hitComponent;            // the use rows and kSoap
    ue_wrap::FVector hitLocation{};
    uint8_t action = 0;
    ue_wrap::CachedObjRef lookAt;                  // the three action rows and kSoap
    ue_wrap::reflection::FName holdPropName{};     // kSoap
    bool replayHeld = false;                       // dev: HoldReplayUntilGone parked the replay of a grant
};

// Opens the record and sends the request (the watches call it). False when a record is open already, a double press
// that shows nothing, or when the send failed: the record then stays closed and the line "The host could not be asked;
// try again." shows. Ids skip 0. Game thread.
bool Ask(const Pending& filled, coop::net::PropActFamily family);

namespace dev {   // called only from src/coop/dev/ (the act drills); compiled always, inert unless called
struct LastReply { uint16_t id = 0; uint8_t granted = 0, reason = 0; uint32_t count = 0; };
LastReply LastStaleReply();          // the last reply that matched no record (a forged request's answer), and how many
void DestroyInsideNextReplay(void* actor);   // held as a CachedObjRef; destroyed after the verb, inside the latch
uint16_t PendingId();                // the open record's id, 0 when none (ids skip 0)
void HoldCommits(bool on);           // while on, a commit is queued in send order instead of sent; ReleaseHeldCommits
                                     // sends the queue and the hold stays on until HoldCommits(false)
void ReleaseHeldCommits();
void HoldReplayUntilGone(bool on);   // a grant parks the replay; Tick aborts it once the target's mirror is gone
uint32_t RefusedCount(coop::net::PropActReason reason);   // replies this client took as refused, per reason
uint32_t HostCancels();              // the host's own takes and uses cancelled for a client's reservation
uint32_t LateGrantsAborted();        // grants for an expired request, answered with an abort
size_t ExpiredPending();             // requests whose wait ran out and whose reply has not come in
bool NotReadyShown();                // the "Not ready yet" line has shown this session (its first show is never limited)
}  // namespace dev

}  // namespace coop::prop_act_request
