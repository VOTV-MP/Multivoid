// coop/creatures/wolfgang_sync.h -- Wolfgang is the host's.
//
// The cave's spawner (antibreatherSPawner_C) mints Wolfgang (centipedeHead_C) in its `cencipede` when a pumpkin lies
// at the cave, on its own ten-second loop on every machine; her begin play mints her body (centipedeSegmet_C) and her
// tick drives it; her young (wolfgangJr_C, their centipedeSegmet_jr_C) and her nest (eggyWolfgang_C, which mints
// parentWolfgang_C) follow. Each machine had its own, and the host's reached no one. HOST: the spawner, the heads and
// the nest are catch sources, every class crosses as a world-actor mirror, and the save's are enrolled here at their
// births (coop/world/ambient_births). CLIENT: its spawner's `cencipede` is refused, and its own copies (a save's, or a
// mirror's begin play's body) end at their births, so it shows the host's alone. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::wolfgang_sync {

void Install(coop::net::Session* session);

// The watch, retried until registered; the births the hub announced. Game thread, once a tick.
void Tick();

void OnDisconnect();

}  // namespace coop::wolfgang_sync
