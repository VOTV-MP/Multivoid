// coop/world/basalt_pillar.h -- the Basalt Pillars, the host's field shown to every player.
//
// The pillars begin as a buried prop (prop_hexpillar_C). Its digUp mints a hexpillar_C at its place and destroys the prop.
// A pillar's tick raises it (its `a`, 0 to 1, which upd() turns into the mesh's height and width) and, on a roll that grows
// with `a`, seeds a neighbour 150 cm off, towards player 0; a pillar's `i` counts the generations left. Each peer's pillars
// would spread their own way, so the field is the host's:
//   - hexpillar_C rides the world-actor lane (each mint is sent; a joiner's save holds the standing ones, adopted as copies);
//   - a client's pillar never ticks (coop/world/spawn_authority refuses it: the one tick both raises and seeds), and the
//     host sends each pillar's `a` as it rises, which the copy shows through the game's own upd();
//   - a client's dig is refused on its own copy of the prop and asked of the host (PropIgniteIntent, verb kVerbDig).
// The growth rides AmbientFixtureState with the family kPillarGrowth: the pillar by its world-actor element id, `a` in x.
// Not carried: a client's hit on a pillar (the game breaks pillars with melee; a copy's break stays that client's).
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
}  // namespace coop::net

namespace coop::basalt_pillar {

// PropIgniteIntent's verb for a dig (0 and 1 are the salt rocket's).
inline constexpr uint8_t kVerbDig = 2;

void Install(coop::net::Session* session);

// HOST: every two seconds, the pillars whose `a` moved are sent.
void Tick();

// HOST: a PropIgniteIntent as it came. True when its verb is the dig, which is then run here; false leaves the message to
// the salt rocket's lane. The sender slot is the transport's.
bool TakeDigIntent(int senderSlot, const uint8_t* bytes, uint16_t len);

// CLIENT: one pillar's `a`.
void OnGrowth(const coop::net::AmbientFixtureStatePayload& p);

// HOST: a peer's world is ready: every pillar's `a` is sent again.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

}  // namespace coop::basalt_pillar
