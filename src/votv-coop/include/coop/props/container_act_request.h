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
// No family's replay is built in this package: a grant is answered with an abort until a family brings its own.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/container_act_wire.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_act_request {

void Install(coop::net::Session* session);
void Tick();            // sends the commits the transport refused again; the 10 s expiry of the open record
void OnDisconnect();    // the pending record, the expired list, the unsent commits, both seq counters, the line timers
void OnReply(const coop::net::ContainerActReplyPayload& p);

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
}  // namespace internal

}  // namespace coop::props::container_act_request
