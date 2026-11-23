// coop/world/obelisk_sync.h -- the warning obelisk falls and lands on every player's machine.
//
// obelisk_C (day 25) is spawned by its event on the host alone. Its begin play drops it from high above on a timeline;
// its landing shakes every camera near it, fires its particles, flags its radar point (the base alarm), pushes the
// props round it, flings 70 rocks, leaves the standing prop_obelisk_C, punches the main player and hides its own mesh.
// The rocks and the standing obelisk are host births and cross as props; a client saw them appear, and no fall, shake
// or punch. HOST: the obelisk's begin play, while it has not fallen, is sent (EventCue 108 its yaw, then 107 its
// place). CLIENT: spawns its own copy there, turned as the host's, so the fall and landing play for its own player as
// the game's do (its own camera, its own punch, its own event count); the copy's push is turned off, and the props its
// landing makes are ended at their birth, since the host's cross. A joiner loads the obelisk the save holds; one that
// joins while it falls is sent its begin play too, and its save's own obelisk by the copy ends. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::obelisk_sync {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueObelisk = 107;
// Its yaw, sent just before its place (x = the yaw).
inline constexpr uint32_t kCueObeliskYaw = 108;

void Install(coop::net::Session* session);

// The watch and the hook, retried until registered, and a client's copy's births ended. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::obelisk_sync
