// coop/dev/live_store_readout.h -- dev-only, read-only observability for the LIVE personal
// inventory store (ini live_store_readout=1, off by default). Not behind the developer gate,
// since it neither sends nor mutates cross-peer state; the ini key is the only one.
//
// The save-side projection (saveSlot.inventoryData) is what our own lane reads, and it is a
// poor instrument: the field has no gameplay reader in the cooked game -- the projection copy
// writes it, the save-slot menu's repair routine reads it -- and on a client it never refreshes
// during a session, because save_block holds the gamemode's disableSave true. So what a player
// carries was not observable on a client, and the live-versus-projection gap could be counted
// but never attributed to a record. This prints that gap BY CONTENT.
//
// Read-only by construction: it calls ue_wrap::inventory::ReadLivePersonalStore and ReadAll,
// field reads both, and no UFunction. An earlier probe here called mainGamemode::saveObjects to
// "look", which refreshed inventoryData, drove the inventory sync's poll into a stream and a
// host persist, and rewrote a player's blob with a state no organic run produces.

#pragma once

namespace coop::dev::live_store_readout {

// Poll the live store + the projection and log on CHANGE (no-op unless ini
// live_store_readout=1). Game thread.
void Tick();

}  // namespace coop::dev::live_store_readout
