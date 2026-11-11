// coop/cheats/item_catalog.h -- every prop the game can spawn, read from the game's own data: the list its prop
// renderer draws icons for (the gamemode's propRenderer Names, the same table the inventory and the spawn menu key
// their props by), never a list kept here. The cheat panels search it and the host resolves every name a player
// asks for against it.
//
// The list lives on game objects, so it is read on the game thread and published as an immutable snapshot the render
// thread copies out: a refresh is posted by RequestRefresh (the panel asks every frame it is open, the module limits
// itself) and a reader never waits. The game fills the list some seconds after a world loads; until then the snapshot
// is empty and the panel says it is loading.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace coop::cheats::item_catalog {

// The names the game lists, ASCII, sorted case-insensitively, without duplicates; null before the first refresh
// landed. Any thread.
std::shared_ptr<const std::vector<std::string>> Names();

// Post a refresh on the game thread: every two seconds until two reads in a row agree (the game fills its list some seconds
// after a world loads, and the first read can land mid-fill), then never again (the list does not change within a run). Any thread; cheap to call every frame.
void RequestRefresh();

// The list's own spelling of `typed`, case-insensitive; false when the game lists no such prop. Needs a refresh to have
// landed. Game thread (the host's judge).
bool Resolve(std::string_view typed, std::wstring& canonical);

}  // namespace coop::cheats::item_catalog
