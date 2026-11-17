// coop/cheats/item_catalog.h -- every prop the game can spawn, read from the game's own data: the list its prop
// renderer draws icons for (the gamemode's propRenderer Names, the same table the inventory and the spawn menu key
// their props by), never a list kept here. The cheat panels search it and the host resolves every name a player
// asks for against it.
//
// The list lives on game objects, so it is read on the game thread and published as an immutable snapshot the render
// thread copies out; a reader never waits. The game fills the list some seconds after a world loads and then calls
// intComs_propRenderer_finishProps on its gamemode: a script-gate watch on that call reads the list the moment it is
// complete, in every world, and a list read in another world is not served. A read at the first ask of a world covers
// a world whose fill has already passed. The catalog holds the script gate from the first ask of a world until the
// list is read, then gives it back (a coop session holds it anyway; a solo game does not).

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace coop::cheats::item_catalog {

// The names the game lists, ASCII, sorted case-insensitively, without duplicates; null until the list of the current
// world has been read. Any thread.
std::shared_ptr<const std::vector<std::string>> Names();

// Ask for the list of the current world: one read is posted per world (the fill's watch covers a list not complete
// yet), so a panel may call it every frame. Any thread.
void RequestRefresh();

enum class Lookup : unsigned char {
    Found,     // the list's own spelling is in `canonical`
    Unknown,   // the list is read and has no such prop
    Unfilled,  // the game has not filled its list in this world yet: no answer about the name can be given
};

// The list's own spelling of `typed`, case-insensitive. Reads the list at once when this world's is not read yet.
// Game thread (the host's judge).
Lookup Resolve(std::string_view typed, std::wstring& canonical);

}  // namespace coop::cheats::item_catalog
