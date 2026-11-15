// coop/world/level_fixture_sync.h -- the level's own wandering fixtures, kept where the host's stand.
//
// Four level-placed actors decide for themselves, on each peer, where they stand or whether they stand at all:
//   church_C       the Cursed Church: moves to a random point while unseen and rings its bell seven times.
//   teleBooth_C    the Telephone Booth: hidden until its two-hour timer shows it, and one time in ten moves it while unseen.
//   sirenPost_C    the Siren Pole: walks along the ground whenever it was not rendered for a minute.
//   hallwayDoor_C  the Hallway White Door: destroyed at begin play unless the save was loaded at 03:33.
// The host's are the world's: it sends where each stands, whether it shows and whether it lives, and a client's copy follows;
// a client's own move is refused (coop/world/spawn_authority). A copy the client is looking at waits until it looks away, as
// the game moves these only unseen; the church's copy rings when it takes a new place.
// Wire: AmbientFixtureState, family kLevelFixture -- the class by row number, the actor by its level name, the pose, alive and
// shown. A joiner is sent every one and then a census end, after which an unnamed Hallway White Door is destroyed. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
}  // namespace coop::net

namespace coop::level_fixture_sync {

// PropIgniteIntent's verb for a client's footprints show() (0 and 1 are the salt rocket's, 2 the basalt dig).
inline constexpr uint8_t kVerbFootprints = 3;

void Install(coop::net::Session* session);

// HOST: a client's PropIgniteIntent of verb kVerbFootprints: the host's own show(). False when the verb is not this lane's.
bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len);

// HOST: reads its fixtures once a second and sends what changed. CLIENT: applies what waits for an unseen copy.
void Tick();

// CLIENT: one AmbientFixtureState of the family kLevelFixture.
void OnState(const coop::net::AmbientFixtureStatePayload& p);

// HOST: a peer's world is ready: every fixture is sent again, then the census end.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

}  // namespace coop::level_fixture_sync
