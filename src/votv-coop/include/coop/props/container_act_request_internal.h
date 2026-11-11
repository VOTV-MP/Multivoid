// coop/props/container_act_request_internal.h -- the client's container act state, opened to the seats that share it:
// container_act_request.cpp (the take seat, the replies, the commit queue), container_act_put.cpp (hold and run),
// container_act_drag.cpp (a drag onto the inventory screen's canvas) and container_act_stack.cpp (part of a stack, a merge
// of two). They share the open puts, the takes waiting for an answer, the replay latch, the commit queue and the player's
// lines. Not an API: only those files include it. Game thread.

#pragma once

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_wire.h"

#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::props::container_act_request::detail {

// What the player did to put an entry in: ran with an item in hand (the container's playerUsedOn), or dragged an entry from
// the bag onto the screen's canvas (the inventory's addItemAdvanced).
enum class PutKind : uint8_t { HoldAndRun, Drag };

// An open put: the target it goes into, where the entry comes from, and the entry's identity as the host was asked.
//   HoldAndRun: `container` is the container actor and `source` the actor in the hand; `entry` is unused.
//   Drag: `container` is the inventory component the entry is filed in, `source` the bag's item widget it comes from, and
//   `entry` the entry as the component would store it (its place on the canvas set).
struct Pending {
    uint16_t id = 0;
    uint32_t eid = 0;
    uint64_t sentMs = 0;
    PutKind kind = PutKind::HoldAndRun;
    ue_wrap::CachedObjRef container;
    ue_wrap::CachedObjRef source;
    uint64_t hash = 0;
    std::wstring name;
    ue_wrap::inventory_item::Item entry;
};

// Sends the put-record request `r` with a fresh id and opens `p` under it. A hold-and-run put is one at a time; a drag's
// are up to kMaxPuts. False, with the player's line shown (none for a double press), when it is not asked: nothing opened.
bool Ask(coop::props::container_act_wire::Request r, Pending p);

// The put seats' shared ask: `rec` into the target `eid`, the verb at the seat cancelled whatever came of it. A record with
// no name is never asked, nor is one too large for its stored form to pack.
ue_wrap::script_gate::Verdict AskPutRecord(PutKind kind, uint32_t eid, void* container,
                                           const ue_wrap::inventory_item::Item& rec, void* source);

// The replay's apply latch for the length of one scope: the verb watches see InReplay() while it stands.
struct Latched {
    Latched();
    ~Latched();
    Latched(const Latched&) = delete;
    Latched& operator=(const Latched&) = delete;
};

// A take-like act that has run on this peer and waits for the host's answer: a whole entry out, part of a stack out, or a
// merge of two entries. `copy` is the item widget the drag filed in the player's bag, taken out again if the host refuses.
struct Take {
    uint16_t id = 0;
    uint32_t eid = 0;
    uint8_t family = 0;
    uint64_t sentMs = 0;
    ue_wrap::CachedObjRef copy;
    // A take of part of a stack files its copy after the call that is asked about returns: the component the part left
    // and the entry the copy stands for find it (AttachCopies) once the drag's call has.
    bool awaitCopy = false;
    ue_wrap::CachedObjRef from;
    ue_wrap::inventory_item::Item part;
};

// Sends `r` under a fresh id and queues `t` under it. False, with the player's line shown and nothing queued, when the
// transport refused the request or this peer already waits on kMaxTakes.
bool AskTake(coop::props::container_act_wire::Request r, Take t);

// The copies of the takes waiting for one: the drag has filed them in the bag by now.
void AttachCopies();

// A merge's source entry leaves its inventory with the merge: the removal that follows is the merge's, not a take of its
// own. NoteMergeSource names the entry (its inventory and record hash) and whether the merge was asked of the host; the
// take seat's next removal of that entry consumes the note, and a tick drops one that was not consumed. A removal whose
// merge was not asked is refused, so the entry is not taken without the merge.
enum class MergeSource : uint8_t { None, Asked, NotAsked };
void NoteMergeSource(void* inventory, uint64_t recordHash, bool asked);
MergeSource ConsumeMergeSource(void* inventory, uint64_t recordHash);

// A commit is queued and sent, and sent again until the transport takes it. SendCommit is a commit without a record.
bool QueueCommit(const container_act_wire::Commit& c);   // false, nothing queued, when it does not pack
void SendCommit(uint16_t id, uint32_t eid, uint8_t committed);

// The player's lines. Each is shown at most once per window.
void ShowRefusal(uint8_t reason);
void ShowBadReason();
void ShowNotAsked();
void ShowNotReady();
void ShowBusy();

// The session while a link to the other peers is up, else null. A watch fires on the game thread.
coop::net::Session* LinkedSession();

// The host's own put beside a client's hold: Cancel (and the busy line) when `container` holds a client hold of any
// kind; Run otherwise, and for anything that is not a shared container.
ue_wrap::script_gate::Verdict HostPre(void* container);

// Is `container` one the host shares? Own: not a world container, or a client's own (a thrown one whose birth slice has
// not gone, or one in an announced world with no element): the act runs natively. NotReady: a host container this
// client has not bound yet: the act is cancelled with the not-ready line. Shared: `eid` is its element.
enum class Target : uint8_t { Own, NotReady, Shared };
Target SharedTarget(void* container, uint32_t& eid);

// The same for an inventory component: a world container's, the delivery drone's cargo (`eid` is then
// container_target::kDroneCargoEid) or an ATV's trunk (`eid` is its container_target::IsAtvTrunk id). Any other
// component is Own.
Target SharedTargetOfInventory(void* inventory, uint32_t& eid);

// A seat's watch, appended to the request's watch table, and the replay of a granted hold-and-run put or drag.
struct WatchRow {
    const ue_wrap::facts::Function& row;
    int tag;
    ue_wrap::script_gate::PreFn pre;
    ue_wrap::script_gate::PostFn post;   // null for a seat with no post
};
const WatchRow& PutWatch();
const WatchRow& DragWatch();
const WatchRow* StackWatches(size_t& count);
void ReplayPut(const Pending& p);
void ReplayDrag(const Pending& p);
void ResetPutLines();   // the once-per-session lines of the seats: said again in the next session

}  // namespace coop::props::container_act_request::detail
