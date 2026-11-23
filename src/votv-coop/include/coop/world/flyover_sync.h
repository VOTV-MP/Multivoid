// coop/world/flyover_sync.h -- the story's sixth week's flyovers on every player's machine: the peeping UFO, the Rozital
// mothership and the drop saucers. The dispatcher spawns them on the host alone; the world-actor lane mirrors each as a
// parked copy on the host's actor pose (coop/creatures/npc_world_enum lists them), and this lane carries the rest.
//   - rozitBorg_C flies its `ships` on its own tick by `alpha`, its actor at the origin. A client's copy ticks, and the
//     host sends its alpha every 2 s (EventCue 115), so a joiner's copy is where the host's is.
//   - morningUfo_C waits hidden until a player enters its box, peeps, and flies away once it sees player 0. HOST: that
//     look reads the player nearest it (the served-player seam), and its show is sent (113). CLIENT: its box refused.
//   - ufoDropper_C's kinds wait hidden for a player in their box (or forceDeploy), then a timeline lowers the saucer,
//     blinks the base's lights, drops the body or the car and ends. HOST: the deploy is sent (114). CLIENT: the copy's
//     begin play, box, deploy, drop and end refused; at 114 it shows and plays its timeline, its tick on for the blink.
//     A saucer loaded from a client's own save ends where the host's copy stands or the host has none (the host's
//     saucers' places, to a joiner at its world-ready, 116, then an end marker). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::flyover_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCueMorningShow = 113;    // the peeping UFO's place
inline constexpr uint32_t kCueDropperDeploy = 114;  // the drop saucer's place
inline constexpr uint32_t kCueBorgAlpha = 115;      // x = the mothership's alpha
inline constexpr uint32_t kCueDropperLive = 116;    // a drop saucer the host has, its place, to a joiner (z -1e9: the end)

void Install(coop::net::Session* session);

// The watches, retried until registered; the host's served looks and its mothership's alpha. Game thread, once a tick.
void Tick();

// CLIENT, the world-actor lane's: a mirror was made, and is ending. Game thread.
void OnMirrorMaterialized(void* actor);
void OnMirrorEnding(void* actor);

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST: its saucers' places, to a joiner at its world-ready (coop/session/connect_replay). Game thread.
void SendJoin(int slot);

void OnDisconnect();

}  // namespace coop::flyover_sync
