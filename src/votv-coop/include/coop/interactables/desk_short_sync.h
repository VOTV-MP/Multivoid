// coop/interactables/desk_short_sync.h -- the base computer's short circuit is the host's.
//
// A liquid that reaches the desk shorts it on the peer whose liquid it was (ue_wrap/desk/desk_short): sparks for about
// ten seconds, then the base on solar power. So every peer polls the desk's `shortCircuiting`. A rising edge on the
// host goes to every client (DeskShortState); one on a client goes to the host (DeskShortRequest), which checks the
// sender's world and its reach and runs the desk's own shortCircuit on its copy, whose edge then goes to everyone. A
// client that hears the host's short runs its own copy's, so the sparks and the power-down play on every peer; a peer
// whose desk already shorts is left alone, as the event's own guard leaves it. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskShortPayload;
}  // namespace coop::net

namespace coop::desk_short_sync {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The poll (every 100 ms) and the host's served requests. Game thread.
void Tick();

// CLIENT: the host's short.
void OnState(const coop::net::DeskShortPayload& p);

// HOST: a client's short, which the host checks and runs on its own desk.
void OnRequest(int senderSlot, const coop::net::DeskShortPayload& p);

// The session ended: the counts are said and everything is forgotten.
void OnDisconnect();

}  // namespace coop::desk_short_sync
