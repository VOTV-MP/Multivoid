// coop/interactables/desk_short_sync.h -- the base computer's short circuit is the host's.
//
// A liquid that reaches the desk shorts it (ue_wrap/desk/desk_short): sparks for about ten seconds, then the base on
// solar power. The game announces the short by calling the desk's own shortCircuit, so a script-gate watch on that body
// (by class and name, the shape of coop/props/food_tick_gate) sees every call, from the liquid's Blueprint or ours.
// A client's call is CANCELLED and asked of the host (DeskShortRequest): the host judges the sender (its world, that it
// lives, its reach, a per-slot budget) and runs the desk's own shortCircuit on its copy, whose end goes to every client
// (DeskShortState), the asker included, so a client's desk sparks only by the host's word. A request the host refuses
// is answered to its sender alone with the host's state (DeskShortState, `on` 0 or 1); an ask nothing answers ages out.
// A client that hears the host's short runs its own copy's; a peer whose desk already shorts is left alone, as the
// event's own guard leaves it, and a state that finds no desk here is held until it resolves, its ten seconds pass or its
// world ends. A client whose world is not yet announced cancels its own short and asks nothing. A joiner that arrives while
// the host's desk shorts is sent the state at its ready edge. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskShortPayload;
}  // namespace coop::net

namespace coop::desk_short_sync {

// Store the session and register the watch on shortCircuit, once for the process. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// Resolve the watch's names until it settles (a dead one is said in one ERROR). Host: serve each sender's queued requests
// at its budget. Client: send its pending ask, age out an ask nothing answered, and apply a held state once the desk
// resolves. Game thread.
void Tick();

// CLIENT: the host's state: its desk shorts (`on` 1) or does not (`on` 0, the answer to a refused ask).
void OnState(const coop::net::DeskShortPayload& p);

// HOST: a client's ask from the wire, its format checked by the dispatcher. Queued per sender and run by Tick.
void OnRequest(uint8_t senderSlot);

// HOST: a joiner's world is up; its copy shorts at once when the host's desk is shorting now.
void SendCurrentToSlot(int slot);

// The session ended: the counts are said and everything is forgotten.
void OnDisconnect();

// What this session did, for the drill. Game thread.
struct Counts {
    uint32_t sent = 0;      // HOST: its short, told to every client
    uint32_t asked = 0;     // CLIENT: its cancelled short, asked of the host
    uint32_t served = 0;    // HOST: a client's ask that started the host's short
    uint32_t refused = 0;   // HOST: a client's ask the host judged against
    uint32_t answered = 0;  // HOST: a state sent to one sender alone (a refusal, or a desk already shorting)
    uint32_t dropped = 0;   // HOST: an ask past the sender's queue, unanswered
    uint32_t applied = 0;   // CLIENT: the host's short run on this copy
    uint32_t heard = 0;     // CLIENT: the host's answer that its desk does not short
    uint32_t aged = 0;      // CLIENT: an ask nothing answered in time
    uint32_t held = 0;      // CLIENT: a state that waited for the desk to resolve
    uint32_t lapsed = 0;    // CLIENT: a held state dropped: its ten seconds passed, or its world changed
    uint32_t stale = 0;     // HOST: an ask served past its ten seconds, dropped unserved
    uint32_t replayed = 0;  // HOST: the state sent to a joiner at its ready edge
};
Counts GetCounts();

}  // namespace coop::desk_short_sync
