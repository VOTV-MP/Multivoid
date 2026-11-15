// coop/creatures/boar_sync.h -- the gray boars are the host's, and its motion of each reaches every peer.
//
// A boar (grayboar_C, and grayboar_overcharged_C below it) is a prop with a brain: a hovering physics body its own
// graph pushes along a path its nav pawn finds. The boar war (boarInvasion_C) throws them out by the treehouse, and
// in a combat save the night spawner (grayBoarSpawner_C) rolls them near the player. Each is born on the host and
// crosses as a prop; a client's copy runs no decision of its own (coop/world/copy_graph_gate) and a client's night
// spawner is refused (coop/world/spawn_authority), so the host's boars are the only ones.
//
// What moved a copy was nothing: a boar is not held, thrown or hooked, so no lane streamed it. The host claims every
// boar the gamemode lists (mainGamemode_C::allGrayboars, where each boar's begin play puts it) into the driven-prop
// stream (coop/props/prop_drive_host), twice a second; a claim holds while the boar lives, and the stream sends a
// pose only when it moved. Host only.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::boar_sync {

// Host, game thread, every gameplay tick, before prop_drive_host::Pump: the claim pass, twice a second.
void Tick(coop::net::Session* s);

// Forgets the counts it logged.
void OnDisconnect();

}  // namespace coop::boar_sync
