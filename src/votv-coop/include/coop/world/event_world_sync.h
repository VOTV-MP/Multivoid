// coop/world/event_world_sync.h -- the events whose result is a change of the world or the sky, not a creature.
//
// Each is the HOST's: its roll or edge is seen at a script-gate watch and sent as one EventCue, and a client runs the same verb
// on its own level-placed copy; a client's own roll is refused (coop/world/spawn_authority). A joiner is sent what lasts.
//   130 / 131  the Black Hole Sun's two 2D sounds (newsky_C.tp's earthTP_in, and earthTP_out 86 s on). The flip itself rides
//              the sky lane (SkyStatePayload.blackHole, coop/world/sky_sync): `tp` is a 106 s latent chain a second call stacks.
//   132 / 133  the Day-13 ship tower (arirShipAppear_C) shown / hidden: the host's `appear(dir)`, run on the client's tower.
//   134 / 135  the lake's glow (lakeglow_C) on / off: the host's `setActive`.
//   136        the stolen radio tower: a client spawns its own radiotowerPoof_C, which hides the tower unseen by its camera.
//   137        the red phone: a client's panel starts its own ring, ended when its own player walks up.
//   138        a locker holds a head (x,y,z the locker): a client sets the flag on its copy; the open rides the locker lane.
//   139        the meat rain: a client spawns its own event_fleshRain_C, which rains around its own camera.
//   140        a locker's corpse at x,y,z: a client spawns its own lockerCorpse_C there (its gore is not props).
//   141        the Midas UFO (midasUfoTest_C, a spline flyer) starts; x is the host's progress `a`, so a joiner mid-flight sees it
//              where the host's is. The client's own ufo_midas timer is refused. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::event_world_sync {

inline constexpr uint32_t kCueTpIn = 130;
inline constexpr uint32_t kCueTpOut = 131;
inline constexpr uint32_t kCueShipOn = 132;
inline constexpr uint32_t kCueShipOff = 133;
inline constexpr uint32_t kCueLakeOn = 134;
inline constexpr uint32_t kCueLakeOff = 135;
inline constexpr uint32_t kCuePoof = 136;
inline constexpr uint32_t kCueRing = 137;
inline constexpr uint32_t kCueLockerHead = 138;
inline constexpr uint32_t kCueMeatRain = 139;
inline constexpr uint32_t kCueLockerCorpse = 140;
inline constexpr uint32_t kCueMidasUfo = 141;

// The host's watches, registered once; every install tick.
void Install(coop::net::Session* session);

// Every gameplay tick: a client's cue whose target is not born yet is tried again; the host arms its sound seam.
void Tick();

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST, at a joiner's world-ready: what is lasting now (the lake lit, the tower shown, a ring, a vanish, a rain, a head).
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

// Counters for the drills.
unsigned SentCount();

}  // namespace coop::event_world_sync
