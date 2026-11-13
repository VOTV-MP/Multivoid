// coop/interactables/desk_short_sync.h -- the base computer's short circuit is the host's.
//
// A liquid that reaches the desk shorts it on the peer whose liquid it was (ue_wrap/desk/desk_short): sparks for about
// ten seconds, then the base on solar power. The game announces the short by calling the desk's own shortCircuit, so
// a script-gate watch on that body (by class and name, the shape of coop/props/food_tick_gate) sees every call, from
// the liquid's Blueprint or ours. A call that sets the flag on the host goes to every client (DeskShortState); one on
// a client goes to the host (DeskShortRequest), which checks the sender's world and its reach and runs the desk's
// own shortCircuit on its copy, whose call then goes to everyone. A client that hears the host's short runs its own
// copy's, so the sparks and the power-down play on every peer; a peer whose desk already shorts is left alone, as the
// event's own guard leaves it. A joiner that arrives while the host's desk shorts is sent the state at its ready edge
// and shorts from the start. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskShortPayload;
}  // namespace coop::net

namespace coop::desk_short_sync {

// Store the session and register the watch on shortCircuit, once for the process. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// Resolve the watch's names until it settles (a dead one is said in one ERROR), then the host's served requests.
// Game thread.
void Tick();

// CLIENT: the host's short.
void OnState(const coop::net::DeskShortPayload& p);

// HOST: a client's short, which the host checks and runs on its own desk.
void OnRequest(int senderSlot, const coop::net::DeskShortPayload& p);

// HOST: a joiner's world is up; its copy shorts at once when the host's desk is shorting now.
void SendCurrentToSlot(int slot);

// The session ended: the counts are said and everything is forgotten.
void OnDisconnect();

// What this session did, for the drill. Game thread.
struct Counts {
    uint32_t sent = 0, asked = 0, served = 0, refused = 0, applied = 0, replayed = 0;
};
Counts GetCounts();

}  // namespace coop::desk_short_sync
