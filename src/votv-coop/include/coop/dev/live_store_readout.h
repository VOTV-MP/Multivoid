// coop/dev/live_store_readout.h -- DEV-ONLY, READ-ONLY observability for the LIVE personal
// inventory store (ini live_store_readout=1; OFF by default; never ships enabled).
//
// WHY THIS EXISTS. Until 2026-07-24 the only per-player inventory anyone could observe was the
// SAVE-SIDE projection (saveSlot.inventoryData), because that is what our lane reads. The RE that
// day measured two things that make the projection a poor instrument:
//   - `inventoryData` has NO gameplay reader anywhere in the cooked game -- its writers are
//     saveObjects (the projection copy) and a dead legacy function, its only reader is the
//     save-slot menu's repair routine;
//   - a client's projection never refreshes during a session, because save_block holds
//     gamemode.disableSave=true and saveSlot_C::save gates saveObjects on it at op03.
// So "what is the player carrying" was not observable at all on a client, and the live-vs-projection
// gap measured earlier (client 0-vs-6, host 4-vs-5, constant across samples) could be counted but
// never ATTRIBUTED -- nobody could say WHICH record the gap was.
//
// This readout makes the live store observable and prints the gap BY CONTENT, so the standing
// "is the host's gap=1 specifically the taken record?" question is answered by reading a log line
// instead of designing a run.
//
// STRICTLY READ-ONLY. It calls ue_wrap::inventory::ReadLivePersonalStore + ReadAll and formats the
// result. It does not write either store, does not touch the wire, does not persist, and does not
// apply. That boundary is deliberate: everything below reading is gated on the open per-player
// scope questions (votv-per-player-inventory-scope-BRIEF-2026-07-24.md Q1-Q4).
//
// It is also careful not to repeat the instrument mistake this subsystem already made once: the
// earlier projection probe called mainGamemode::saveObjects to "look", which refreshed
// inventoryData, drove player_inventory_sync's poll into a stream + host persist, and rewrote a
// player's on-disk blob with a state no organic run produces
// ([[lesson-probe-side-effects-travel-through-our-own-lanes]]). Nothing here calls a UFunction.
//
// NOT dev_gate-gated (it never sends or mutates cross-peer state); the ini key is the only gate.
//
// ---- STATUS: AS-BUILT (2026-07-24). NOT VERIFIED. -------------------------------------------
// Evidence so far is ONE 60 s autonomous LAN smoke (DLL 65c6b9f726f09494): host read
// GObjStack[0] live=4 proj=4 gap=+0; client read live=4 proj=0 gap=+4, then a CHANGE line to
// live=5 whose new record key matched the destroy-seam + container_selftest extract in the same
// second. That is a smoke, not a hands-on -- it shows the reader resolves and tracks a change on a
// scripted world, and it does NOT show the numbers are right for a human playing normally.
//
// WHAT IS TESTED vs NOT, precisely, so the next reader does not inherit a false green:
//   [tested]   resolve chain (gamemode -> playerContainer -> propInventory -> GObjStack[Index]);
//              the personal ADDRESS ASSERTION (Player!=0) admitting the right component;
//              change-detection (4 silent polls at an unchanged value, then one line when the
//              content actually changed);
//              the live-only diff direction (produced 4 then 5 correctly-named entries).
//   [UNTESTED] the proj-only diff direction. It has printed "(none)" every time, and that zero is
//              currently WORTHLESS as evidence: on the client the projection is EMPTY, so an empty
//              proj-only is a mathematical necessity, not a measurement; on the host the two bags
//              were EQUAL, so both directions are empty for the same trivial reason. There is no
//              known-positive ([[lesson-negative-grep-verify-against-known-positive]]). The
//              scenario that would produce one: an item leaves the LIVE store after the projection
//              last caught up (host takes something -> host saves -> host drops it), so the stale
//              projection holds a record the player no longer carries. Until that is seen, treat a
//              "(none)" on this line as UNINFORMATIVE, not as "nothing is stale".
//   [UNTESTED] behaviour across a level transition / reconnect (the offsets are cached; the
//              saveSlot resolve revalidates, the playerContainer pointer is re-read each call).

#pragma once

namespace coop::dev::live_store_readout {

// Poll the live store + the projection and log on CHANGE (no-op unless ini
// live_store_readout=1). Game thread.
void Tick();

}  // namespace coop::dev::live_store_readout
