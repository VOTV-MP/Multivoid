// coop/world/wanderer_sync.h -- the Anomalous wanderers stand in one place for every player.
//
// Four level-placed entities move by their own random rolls, so each machine moved its own copy: the ghost log
// (log_C: vanishes and comes back at a random point), the telephone booth (teleBooth_C: its timer moves it while off
// screen), the sheet ghost (gost_C: its timers move and hide it) and William the Protector (goat_C: its tick moves
// its stand and hides the goat off its pedestal).
// HOST: they roll; each one's state is sent when it changes, and to a joiner at its world-ready (EventCue 127 which
// and its flags, 128 its turn, 129 its place). The log's vanish and return are sent as they start (130).
// CLIENT: those rolls are refused; each copy takes the host's state, and the log plays the host's vanish. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::wanderer_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCueWhich = 127;   // x = the entity's index, y = its flags (1 actor hidden, 2 part shown)
inline constexpr uint32_t kCueTurn = 128;    // pitch, yaw, roll
inline constexpr uint32_t kCuePlace = 129;   // its place: applies the three
inline constexpr uint32_t kCueVanish = 130;  // the log's vanish: x = 1 it fades out, 0 it comes back

void Install(coop::net::Session* session);

// The watches, retried until registered; the host's sends. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST: the state, to a joiner at its world-ready (coop/session/connect_replay). Game thread.
void SendJoin(int slot);

void OnDisconnect();

}  // namespace coop::wanderer_sync
