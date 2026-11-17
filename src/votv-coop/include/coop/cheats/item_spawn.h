// coop/cheats/item_spawn.h -- spawn props in the host's world by name, beside a player: the one mechanism under the
// host's Items panel and under a player's request for an item.
//
// A prop is spawned the way the game's own spawners spawn one (a deferred spawn of the class
// /Game/objects/prop_<name>), on the host, so the host's prop lane expresses it to every client like any other host
// spawn (coop/props/host_spawn_watcher); this module sends nothing itself. A name is judged against the game's own
// list first (coop/cheats/item_catalog). Game thread, on the host or in a solo game.

#pragma once

#include <string>
#include <string_view>

namespace coop::cheats::item_spawn {

enum class Result : unsigned char {
    Ok,           // every requested prop was spawned
    NotReady,     // the game has not filled its list of props in this world yet
    UnknownItem,  // the game lists no such prop, or its class does not load
    NoPlace,      // the body to place them by has no place yet (no such player, no pose, no world)
    Failed,       // the class loaded and the spawns came to nothing
};

// The name of the class `name` would be spawned as: its own class, or the class the game's list row names. False for a
// name the game does not list, a list not filled yet, and a row that names no class (a plain prop). The host's judge
// tests it against the explosives, so a row that names an explosive's class is refused whatever its name. Game thread.
bool SpawnClassName(std::string_view name, std::wstring& className);

// Spawn `count` props named `name` beside the player in `slot` (0 = the host's own body): when `inFront`, 2 m ahead
// of where that player looks, else at the player. `spawned` gets how many came into the world. Game thread.
Result Spawn(std::string_view name, int count, int slot, bool inFront, int* spawned);

}  // namespace coop::cheats::item_spawn
