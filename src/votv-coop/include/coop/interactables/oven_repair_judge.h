// coop/interactables/oven_repair_judge.h -- the host judges a client's steps of an oven's repair and its repair.
//
// The repair is kitchen_C::fix(), which the repair screen calls once its wiring is done. A client's fix() is held at
// its source: a script-body PRE on `fix` cancels it on a kitchen_C in an announced world and asks the host
// (OvenRepairState, action 1), after sending its steps again (OvenProgressState) so a step the host never took reaches
// it first in the same ordered lane. A refused or lost ask leaves an unfixed oven: nothing to roll back. A client's
// steps stay optimistic on its own screen (an ask per click would put a round trip in every screw) and reach the host
// as asks too: both kinds enter one ordered queue per sender, served one head a tick at a bounded rate. The host
// checks the oven is one it indexes, the sender's body is within reach of the oven's switch box (no pose yet: the head
// waits, then is refused), then the game's order (a screw at any time, the lid with four screws in the host's record,
// the repair with the lid in it). A grant goes into the host's record and screen and on to the other clients, or runs
// the host's own fix(), whose script-gate edge sends OvenRepairState to every peer. The game never takes a step back,
// so a refused step changes nothing. MTA: a client's vehicle entry is a request the server checks, distance included
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018). docs/devices.md carries the lane and its trust line.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::oven_repair_judge {

// What the lane did, for the drill and the session's summary. Game thread.
struct Stats {
    uint64_t asked = 0;            // CLIENT: repairs asked of the host
    uint64_t held = 0;             // CLIENT: fix() calls cancelled for the ask
    uint64_t ranHere = 0;          // CLIENT: fix() calls left to run here (the red arm's)
    uint64_t loadNative = 0;       // CLIENT: fix() calls of its own world's load, run natively
    uint64_t grantedSteps = 0;     // HOST: steps granted
    uint64_t grantedRepairs = 0;   // HOST: repairs granted
    uint64_t already = 0;          // HOST: asks or steps on an oven already fixed (an ask is answered to the asker alone)
    uint64_t refusedReach = 0;     // HOST: out of reach of the switch box (or the box unreadable)
    uint64_t refusedOrder = 0;     // HOST: a lid without four screws, a repair without the lid
    uint64_t refusedBucket = 0;    // HOST: the sender's queue was full
    uint64_t refusedNooven = 0;    // HOST: a key naming no oven the host indexes
    uint64_t refusedNobody = 0;    // HOST: the sender took no pose within the wait
    uint64_t refusedAction = 0;    // HOST: a repair message that was not action 1
    uint64_t refusedEmpty = 0;     // HOST: a steps message with no key or no bit
    uint64_t fixFailed = 0;        // HOST: the granted fix() did not run
};

// Registers the watch on `fix`, once: a refusal (a full gate table) is final, and said. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watch. Host: serves each sender's queue, one head a tick. Game thread.
void Tick(coop::net::Session& session);

// HOST: OvenRepairState from a client, the dispatcher's case, before the oven channel sees it.
void OnAsk(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot);

// HOST: OvenProgressState from a client, routed here by the dispatcher (event_dispatch_signal) on its session.
void OnSteps(coop::net::Session& session, const coop::net::OvenProgressStatePayload& payload, uint8_t senderSlot);

Stats GetStats();

void OnDisconnect();

}  // namespace coop::oven_repair_judge
