// coop/world/soltomia_sync.h -- the garage Soltomia shows and throws its sponge on every player's machine.
//
// soltomiaCleaning_C (the soltoClean event, day 34) is spawned in the garage by its event on the host alone, hidden. Its
// begin play cleans the ATV; a player in its box shows it, pulls the ATV into the garage, shuts the garage door and
// unjams the base's doors; the door opened, it plays its montage, whose throw hides its mesh, throws a
// prop_sponge_solto_C and frees the ATV, and it ends at the montage's end. The ATV, the doors and the sponge reach a
// client through their own lanes; the Soltomia did not, so a client saw its ATV move and a sponge appear from nothing.
// HOST: its box's run, once it is shown, is sent (EventCue 111 its yaw, then 110 its place), and its `opened` (112).
// CLIENT: a copy spawns there, its begin play, box and end refused, and shows as the game's run shows it: its mesh, its
// squeak, its dust and its talk; at 112 it plays the montage, and the sponge its throw makes is ended at its birth,
// since the host's crosses. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::soltomia_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCueSoltomiaShow = 110;   // its place, after its yaw
inline constexpr uint32_t kCueSoltomiaYaw = 111;    // x = the yaw
inline constexpr uint32_t kCueSoltomiaOpened = 112;

void Install(coop::net::Session* session);

// The watches and the hook, retried until registered, and a client's copy's births ended. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::soltomia_sync
