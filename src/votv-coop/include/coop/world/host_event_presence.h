// coop/world/host_event_presence.h -- an event that runs on the host alone still counts on every player's machine.
//
// The game's `activeEvents` count (lib_C::setEvent) is what its threat music, the meta paranoia, plays on: while any
// event is registered, mainGamemode's threatMusic fades the threat ambience in. An event actor that exists on the host
// alone never registers on a client (the vent knocker, kocker_C: it stands at the main computer until someone steps into
// its box), so a client heard none of it. HOST: when the first of its live registrations of such a class begins and
// when the last ends, one EventCue (id 104, x = 1 or 0), and the state to a joiner while one runs. CLIENT: registers one
// stand-in event (its eventer, which never registers itself) through the game's own setEvent, and ends it. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::host_event_presence {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueHostEvent = 104;

void Install(coop::net::Session* session);

// HOST: an event's begin or end, from coop/world/event_active_sync's watch on setEvent, after the game's bookkeeping.
void OnHostEdge(void* sender, bool active);

// HOST: the state to a joiner at its ready edge, while a host-only event runs.
void SendJoin(int slot);

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::host_event_presence
