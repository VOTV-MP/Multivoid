// coop/world/arir_ship_sync.h -- the Ariral ship that appears in the sky is seen by every player.
//
// arirShipAppear_C is a level-placed event trigger: the arirShip event's box (TB_event_arirAppear) runs its appear(true),
// which fades the ship in and lights its radar point; its own tick fades it out once the player looks at it, and its
// timeline's end mails. The event's box runs on the host alone (its other spawns have no lane), so a client's ship never
// appeared (Ghezra/Multivoid#39). HOST: a post watch on appear sends one EventCue when the ship comes up, and to a joiner
// while it is up. CLIENT: runs its own copy's appear(true); the copy's own tick then ends it for that player when that
// player looks, as the game does for whoever sees it. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::arir_ship_sync {

// The EventCue id of the ship's appearance; the cue registry's ids are append-only.
inline constexpr uint32_t kCueArirShip = 103;

// Store the session. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once per pump tick.
void Tick();

// A client's receiver: true when the cue was the ship's (handled or not).
bool OnCue(const coop::net::EventCuePayload& p);

// HOST: the ship's appearance to a joiner at its ready edge, while it is up.
void SendJoin(int slot);

}  // namespace coop::arir_ship_sync
