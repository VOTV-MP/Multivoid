// coop/dev/live_store_readout.h -- dev-only, read-only observability for the LIVE personal
// inventory store (ini live_store_readout=1, off by default). Not behind the developer gate,
// since it neither sends nor mutates cross-peer state; the ini key is the only one.
//
// What a player carries and wears lives on their pawn's inventory component (slotArray_A, canvasInventory and
// slotArray_B, ue_wrap/actors/inventory.h). This prints them BY CONTENT -- class, save key, payload shape -- on change, which is how "the joiner carries the
// host's items under the host's keys" was measured, and how a rejoin is checked against it. Each
// carried record also gets a `values` line of per-group hashes (coop/dev/record_digest.h), since
// the shape cannot say whether a disc or a drive kept the DATA it holds.
//
// Read-only by construction: it calls ue_wrap::inventory::ReadAll, field reads only, and no
// UFunction. An earlier probe here called mainGamemode::saveObjects to
// "look", which drove the inventory sync's poll into a stream and a host persist, and rewrote a
// player's blob with a state no organic run produces.

#pragma once

namespace coop::dev::live_store_readout {

// Poll the carried and worn records and log on CHANGE (no-op unless ini
// live_store_readout=1). Game thread.
void Tick();

}  // namespace coop::dev::live_store_readout
