// coop/props/container_act_request_internal.h -- the client's container act state, opened to the put seats
// (container_act_put.cpp), which share the one pending record, the replay latch, the commit queue and the player's
// lines with the take seat in container_act_request.cpp. Not an API: only the two files of the client half include it.
// Game thread.

#pragma once

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_wire.h"

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>

namespace coop::props::container_act_request::detail {

// The one open act. A take keeps its slot and the record's index and hash; the container is every family's.
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
void QueueCommit(const container_act_wire::Commit& c);
void SendCommit(uint16_t id, uint32_t eid, uint8_t committed);

// The player's lines. Each is shown at most once per window.
void ShowRefusal(uint8_t reason);
void ShowBadReason();
void ShowNotAsked();
void ShowNotReady();

}  // namespace coop::props::container_act_request::detail
