// coop/props/container_act_request.h -- the client's half of an act on a world container: the act is asked of the
// host first (Ask), the host's reply is taken (a refusal shows a line, a grant is answered with the commit that ends
// the host's hold), and a commit the transport refused is sent again until it goes, so a host hold always ends.
// Gameplay layer (principle 7). Game thread.
//
// One act is pending at a time. An honest wait ends in milliseconds; the 10 s bound covers a transport that refused
// the reply. Every request whose wait ran out is kept until the host answers it, and a grant that comes after the
// bound is answered with an abort, so the host's escrow never outlives the client's wait; one older than the host's
// 30 s hold deadline is dropped, since the host has ended that hold by itself. A refused commit is sent again every
// 250 ms at most.
//
// The take family has its replay: a client's press of an inventory-screen slot is cancelled at the slot's pressButton
// and asked of the host; on a grant the same slot is pressed again under the replay latch, if it still names the
// record the host granted, and the commit says whether the record left the container. Any other family's grant is
// answered with an abort until it brings its own replay.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/container_act_wire.h"

#include <cstddef>
#include <cstdint>

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
// The replay presses only the slot that asked: live, its screen still showing the same container, the same id, and its
// record still hashing what the host granted. A failed read or an id past the end is hashMatches = false.
TakeCheck CheckTake(bool slotLive, bool screenShowsContainer, bool sameId, bool hashMatches);
// The commit after the press. 0 (abort) only when the verb did not run, or it ran and took nothing (the count read
// back and did not fall); else 1 (done): a press that ran and whose count could not be read back counts as done, since
// an abort there would give back a record the player may hold.
uint8_t TakeCommitted(bool pressed, bool afterRead, size_t countBefore, size_t countAfter);
}  // namespace internal

namespace dev {   // the container act drills read it; OnDisconnect says it once; compiled always
uint32_t TakeDoneUnread();   // takes committed done although the press's count could not be read back
uint16_t PendingId();        // the open act's id, 0 when none is open

// The last take commit this peer sent: the act's id, what it said (1 done, 0 abort), and how many take commits this
// session has sent (0: none yet). Reset at the session's end.
struct Take {
    uint16_t id = 0;
    uint8_t committed = 0;
    uint32_t count = 0;
};
Take LastTake();
}  // namespace dev

}  // namespace coop::props::container_act_request
