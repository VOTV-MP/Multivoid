// coop/world/easter_hunt.h -- the Easter hunt, one hunt for every player, the host's (docs/easter-hunt.md).
//
// In season (`mainGamemode.isEaster`) the old well's easterZone_C counts thrown carrots; at seven it plays its rocket animation
// and mints the Ominous Easter Bunny (ominousBunny_C). The first talk's end runs `fin` (an egg at every easterEggSpawn_C, the
// ears and a note), a later one `customEvent` (them again); each egg used on the bunny (`playerUsedOn`) is struck off, and the
// last starts the closing talk, which grants `eastercomplete` and destroys the bunny. A bunny destroyed early mints an angry one.
// On every peer this ran on its own copies. Here it is the host's:
//   - the zone counts on the host alone; its animation start is a cue, and a client's zone plays it without minting a bunny;
//   - the bunny rides the world-actor lane; a copy mints no egg, item or angry bunny (coop/world/spawn_authority), and its `fin`,
//     `customEvent` and an egg's `playerUsedOn` are asked of the host (PropIgniteIntent verbs 4, 5, 6);
//   - the host sends the bunny's flags and its active, shown talk object (AmbientFixtureState, kEasterBunny), so a copy offers
//     the same talk and starts its own closing talk when the last egg is in;
//   - the closing talk's end on the host is a cue at which a client grants its own profile the achievement.
// The talks themselves are each player's own reading. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::easter_hunt {

// EventCue ids (the cue registry's ids are append-only).
inline constexpr uint32_t kCueZoneStart = 161;   // the well's animation began on the host
inline constexpr uint32_t kCueComplete = 162;    // the closing talk ended on the host

// PropIgniteIntent verbs (0 and 1 the salt rocket, 2 the basalt dig, 3 the footprints).
inline constexpr uint8_t kVerbFin = 4;      // the first talk ended on a client's copy
inline constexpr uint8_t kVerbRepeat = 5;   // a later talk ended on a client's copy
inline constexpr uint8_t kVerbEgg = 6;      // a client used an egg on its copy: the egg by its prop key

void Install(coop::net::Session* session);

// HOST: the bunny's state, twice a second, when it changed. CLIENT: a state waiting for its copy, the zone's tail.
void Tick();

// CLIENT: a cue of this lane. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// CLIENT: the bunny's state (family kEasterBunny).
void OnState(const coop::net::AmbientFixtureStatePayload& p);

// HOST: a PropIgniteIntent as it came. True when its verb is this lane's.
bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len);

// HOST: a peer's world is ready: the bunny's state is sent again.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

// [dev] A drill's reflected call of the bunny's `fin`, `CustomEvent` or `playerUsedOn` on a client's copy stands for the game's
// own (a talk's end, a use), so the lane judges it as one. Off outside a drill.
void SetDrillCallsCount(bool on);

}  // namespace coop::easter_hunt
