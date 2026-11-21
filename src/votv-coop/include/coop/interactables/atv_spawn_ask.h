// coop/interactables/atv_spawn_ask.h -- a client's spawn-menu ATV is the host's to make.
//
// The spawn menu's ATV row spawns an ATV_C on the machine of the player who took it. On a client that ATV stayed there:
// the ATV lane announces the host's ATVs only, so the other players never saw it (Trello: ATV visibility desync). A
// CLIENT now removes the ATV its own player's spawn verb makes, on the next tick, and asks the host (AtvSpawnAsk) with
// its place and yaw. The HOST judges the asker (world ready, the place finite and within 20 m of the asker's body, one
// ask per 3 s) and spawns an ATV_C there, which the ATV lane's own AtvSpawn brings to every peer, the asker's included.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AtvSpawnAskPayload;
}  // namespace coop::net

namespace coop::atv_spawn_ask {

void Install(coop::net::Session* session);

// CLIENT: `atv` was just born of this client's player's own spawn verb (prop_spawn_authoring). It is asked of the host
// and removed here on the next Tick. Called from the client's finish-spawn seam (coop/props/prop_drop_intent).
void OnPlayerSpawnedAtv(void* atv);

// The removals the seam queued.
void Tick();

// HOST: a client's ask.
void OnAsk(coop::net::Session& session, const coop::net::AtvSpawnAskPayload& p, int slot);

void OnDisconnect();

}  // namespace coop::atv_spawn_ask
