// coop/props/prop_pry_host.h -- the host's judge of a client's pry (coop/props/prop_pry_sync carries the lane).
//
// A client's crowbarOpen on a pryable prop is cancelled there and asked here (PryAsk). The ask is queued per sender at a
// bounded rate, as a door's verb is (coop/interactables/door_verb_intent), and judged in the order the checks cost: the
// key names a pryable prop on this copy; the sender's puppet is in reach of it, which a sender whose first pose has not
// arrived waits for; the sender's hand held a crowbar last (the prying crowbar destroys the held one as it goes in, so the
// hand is empty by now); the prop is not pried in the host's record and this copy is still static or frozen (a reload
// clears the record, not the game's flags). A grant runs the pry on the host's copy and marks it pried, which tells the
// clients. A refusal is counted and said, rate-limited, and gets no answer: the asker's copy never moved. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PryAskPayload;
}  // namespace coop::net

namespace coop::prop_pry_host {

// HOST: a client's ask from the wire, its format already checked by the dispatcher. Queued, and judged by Tick; a full
// queue refuses it. Game thread.
void OnAsk(coop::net::Session& session, const coop::net::PryAskPayload& payload, uint8_t senderSlot);

// HOST: judges one queued ask a tick for each sender, and listens to coop/props/constraint_ties for the BreakConstraint
// calls that report a pried radiator's joints. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Counters for the session-end line and the pried drill.
struct Counts {
    uint64_t granted;     // asks the host ran
    uint64_t refused;     // asks the judge turned down, for any reason
    uint64_t farRefused;  // of those, out of the sender's reach
};
Counts GetCounts();

// Session end: the counters are said and cleared. A sender's queue and its latches live in its slot's state.
void OnDisconnect();

}  // namespace coop::prop_pry_host
