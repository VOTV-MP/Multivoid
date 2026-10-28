// coop/props/container_act_request_internal.h -- the client's container act state, opened to the put seats
// (container_act_put.cpp), which share the one pending record, the replay latch, the commit queue and the player's
// lines with the take seat in container_act_request.cpp. Not an API: only the two files of the client half include it.
// Game thread.

#pragma once

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_wire.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::props::container_act_request::detail {

// A put's route: R2 is the hotbar key with no hovered slot (em_take), R3 is hold and run (playerUsedOn).
inline constexpr uint8_t kRouteR2 = 1;
inline constexpr uint8_t kRouteR3 = 2;

// The one open act. A take keeps its slot and the record's index and hash; the container is every family's. A put keeps
// its route, the record as it was asked (its hash and name), the player's-pane index it came from (R2, with the
// screen that pressed) or the actor in the hand (R3).
struct Pending {
    bool open = false;
    uint16_t id = 0;
    uint32_t eid = 0;
    uint8_t family = 0;
    uint64_t sentMs = 0;
    // A take's address, kept for the replay: the slot that asked, the container it takes from, and the record's index
    // and hash as the host was asked.
    ue_wrap::CachedObjRef slot;
    ue_wrap::CachedObjRef container;
    uint16_t index = 0;
    uint64_t hash = 0;
    uint64_t grantedMs = 0;   // when the Granted reply was taken here
    bool replayHeld = false;  // dev: HoldReplay parked the replay of a grant
    uint8_t route = 0;
    std::wstring name;
    ue_wrap::CachedObjRef screen;
    ue_wrap::CachedObjRef hand;
};

// The open act, and its end: the record cleared.
Pending& Rec();
void Close();

// The replay's apply latch for the length of one scope: the verb watches see InReplay() while it stands.
struct Latched {
    Latched();
    ~Latched();
    Latched(const Latched&) = delete;
    Latched& operator=(const Latched&) = delete;
};

// A commit is queued and sent, and sent again until the transport takes it. SendCommit is a commit without a record.
bool QueueCommit(const container_act_wire::Commit& c);   // false, nothing queued, when it does not pack
void SendCommit(uint16_t id, uint32_t eid, uint8_t committed);

// The put seats say how the open put act stands, for dev::LastPut: the ask, then the replay's end (`index` and
// `replayHash` as dev::Put names them).
void NotePut(const Pending& p, dev::PutOutcome outcome, uint16_t index, uint64_t replayHash);

// The player's lines. Each is shown at most once per window.
void ShowRefusal(uint8_t reason);
void ShowBadReason();
void ShowNotAsked();
void ShowNotReady();
void ShowBusy();
void ShowEmptyFirst();

// The session while a link to the other peers is up, else null. A watch fires on the game thread.
coop::net::Session* LinkedSession();

// The host's own player beside a client's hold: Cancel (and the busy line) when `container` holds a client hold of
// any kind (`anyHold`) or one reserved by a client; Run otherwise, and for anything that is not a shared container.
ue_wrap::script_gate::Verdict HostPre(void* container, bool anyHold);

// Is `container` one the host shares? Own: not a world container, or a client's own (a thrown one whose birth slice has
// not gone, or one in an announced world with no element): the act runs natively. NotReady: a host container this
// client has not bound yet: the act is cancelled with the not-ready line. Shared: `eid` is its element.
enum class Target : uint8_t { Own, NotReady, Shared };
Target SharedTarget(void* container, uint32_t& eid);

// The put seats' watches, appended to the request's watch table, and the replay of a granted put (OnReply's switch).
struct WatchRow {
    const ue_wrap::facts::Function& row;
    int tag;
    ue_wrap::script_gate::PreFn pre;
};
const WatchRow* PutWatches(size_t& count);
void ReplayPut();
void ResetPutLines();   // the once-per-session lines of the seats: said again in the next session

}  // namespace coop::props::container_act_request::detail
