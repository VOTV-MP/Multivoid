// coop/props/container_act_request.h -- the client's half of an act on a shared inventory (a world container, or the
// delivery drone's cargo): the act is asked of the host (Ask), the host's reply is taken (a refusal shows a line, a grant
// is answered with the commit that ends the host's hold), and a commit the transport refused is sent again until it goes,
// so a host hold always ends. Gameplay layer (principle 7). Game thread.
// Takes are pending together, and so are a drag's puts; a hold-and-run put is pending alone. A request whose wait ran out
// (10 s) is kept until the host answers it, and a grant that comes after the bound is answered with an abort; one older
// than the host's 30 s hold deadline is dropped. A refused commit is sent again every 250 ms at most.
// The take: the inventory screen moves an entry out of a shared inventory with the component's removeInventoryItem, after
// it has filed the entry's copy in the player's bag. The call runs at once on this peer and is asked of the host in the
// same step; the host takes the entry out into escrow and grants, and the commit ends the escrow. A refusal gives the
// entry back: the copy leaves the bag again and the host's truth of the inventory is sent. A take of part of a stack and
// a merge of two entries are asked the same way (container_act_stack.cpp). The put-record family has a replay: the verb
// is cancelled at its seat and asked of the host with the record; on a grant it runs again under the replay latch if it
// still names the record asked, and the commit says where the inventory stored it (container_act_put.cpp for the
// container's playerUsedOn, container_act_drag.cpp for an entry dropped on a canvas). Any other family is aborted.

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

void Install(coop::net::Session* session);   // also registers the take and put watches
void Tick();            // settles the watches; sends the commits the transport refused again; the 10 s expiry of the
                        // pending takes and the open put
void OnDisconnect();    // the pending takes, the open put, the replay latch, the expired list, the unsent commits, both
                        // seq counters, the line timers
void OnReply(const coop::net::ContainerActReplyPayload& p);

// True while a granted act's replay runs on this peer; the verb watch reads it. Game thread.
bool InReplay();

namespace internal {   // pure: the selftest drives it
enum class ReplyAction : uint8_t { Stale, Refused, BadReason, Granted };

// !pending or id != pendingId -> Stale; granted == 1 && reason == 0 -> Granted;
// granted == 0 && 1 <= reason < kContainerActReasons -> Refused; else BadReason.
ReplyAction DecideReply(bool pending, uint16_t pendingId, const coop::net::ContainerActReplyPayload& p);
const char* ReplyActionName(ReplyAction a);   // "stale" "refused" "bad-reason" "granted"

enum class PutCheck : uint8_t { Replay, Abort };
// R3 (hold and run) replays only for the actor that asked: the container live, that actor still the one in the hand,
// and its own record still hashing what was asked.
PutCheck CheckPutR3(bool containerLive, bool handIsAsked, bool hashMatches);
// R3's hash of a held actor's entry, at the ask and at the replay: its class, name and save key. The pose and the
// actor's own serialised state change while the player runs, so neither is hashed; the replay also requires the same
// live hand actor, so the identity alone never lets a different item through.
uint64_t PutHandHash(const ue_wrap::inventory_item::Item& record);
// Where a put's record went, from the container's record hashes before and after the put's verb: the index i when
// after is before with one record inserted at i (the first index where they differ, else the end). None when the count
// did not rise by one or the rest does not line up, so the verb stored nothing, or something else did.
std::optional<size_t> StoredIndex(const std::vector<uint64_t>& before, const std::vector<uint64_t>& after);
}  // namespace internal

}  // namespace coop::props::container_act_request
