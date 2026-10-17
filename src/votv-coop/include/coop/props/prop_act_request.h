// coop/props/prop_act_request.h -- the client's half of a take or use of a shared prop: the host's reply is taken (a
// refusal shows a line, a grant replays the verb under a latch and sends the commit) and the destroy seam is told
// which destroy is the replay's. Gameplay layer (principle 7). Game thread.
//
// One act is pending at a time. An honest wait ends in milliseconds, since every host Wait ends on the client's own
// event; the 10 s bound covers a transport that refused the reply, and a grant that comes after it is answered with an
// abort so the host's reservation never outlives the client's wait.

#pragma once

#include "coop/net/protocol.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"   // FName
#include "ue_wrap/core/types.h"        // FVector

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::prop_act_request {

void Install(coop::net::Session* session);
void Tick();                                     // the 10 s expiry of an open record
void OnDisconnect();                             // the pending record, the latch, the expired ring, the last stale
                                                 // reply and the counters cleared
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
    ue_wrap::CachedObjRef lookAt;                  // kSoap
    ue_wrap::reflection::FName holdPropName{};     // kSoap
};

}  // namespace coop::prop_act_request
