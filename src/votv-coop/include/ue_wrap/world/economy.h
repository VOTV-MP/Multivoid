// ue_wrap/economy.h -- player credit balance (saveSlot.Points) accessor + AddPoints.
//
// Resolves AmainGamemode_C -> saveSlot (UsaveSlot_C* @0x04B0, the canonical ONE-per-
// machine store) and reads/writes the int32 Points by reflected field NAME (RE
// 2026-06-03). Game-thread only (UObject access + a UFunction dispatch). Principle 7: NO
// coop/network logic -- coop/balance_sync owns the wire encoding + host-authoritative
// policy. The mainGamemode pointer + the field offsets are resolved by name (cooked
// offsets shift across recooks) and cached; revalidated via IsLive.
//
// CORRECTION 2026-08-24 (bytecode census, security A13/A35): this header used to say
// "AmainGamemode_C::AddPoints @mainGamemode.hpp:447 is the single credit-writer". It is
// NOT. `[V]` mainGamemode's `addPoints` is a THREE-STATEMENT FORWARDER --
// `Default__lib_C->addPoints(Add, self)`. The real and sole writer in the whole cooked
// corpus is **`lib_C::addPoints(int32 Add, Object __WorldContext)`**, which does the
// VictoryIntPlusEquals on saveSlot.points plus the text_points SetText and the
// save_main.stats totals. AddPoints() below still WORKS (it dispatches the forwarder), so
// this is a documentation defect, not a code one -- but it mattered: the false "single
// writer" claim is why a census of OUR AddPoints callers looked like a census of the
// game's earnings. `[V]` All 19 real call sites are EX_LocalVirtualFunction, i.e.
// PE-INVISIBLE, so no ProcessEvent hook can observe an earning; the one choke point is
// lib_C::addPoints and it is reachable only through the 0x45 vm_dispatch substrate
// (docs/COOP_VM_DISPATCH_PLAN.md). That is what blocks host-authoritative economy.

#pragma once

#include <cstdint>

namespace ue_wrap::economy {

// v114 (L7): the live UsaveSlot_C* (gamemode.saveSlot), or nullptr while unresolvable.
// Exposed for sibling ue_wrap accessors of OTHER saveSlot fields (daily_task) so the
// gamemode->saveSlot resolve lives in exactly one place. Game thread.
void* SaveSlotPtr();

// Read the local machine's balance into *out. Returns false (out untouched) if the
// store isn't resolvable yet (still booting / at the menu).
bool ReadPoints(int32_t* out);

// Write the balance DIRECTLY (the client mirror -- no AddPoints side-effects, so a sync
// doesn't fire "credit earned" UI/email). Returns false if unresolved.
bool WritePoints(int32_t value);

// Add `amount` (signed) via AmainGamemode_C::AddPoints -- the proper credit-writer that
// fires the BP UI/email/achievement side-effects. Returns false if unresolved.
bool AddPoints(int32_t amount);

// Repaint the on-screen HUD credit number to the CURRENT saveSlot.Points without changing
// the value or any stat. The client balance mirror writes Points via WritePoints (side-
// effect-free), but the HUD number (mainGamemode.playerInterface.text_points) is push-
// updated ONLY by the BP credit-writer's SetText -- nothing re-evaluates it per frame -- so
// a direct field write leaves the displayed number frozen (the "host +1000 didn't show on
// the client" bug). Call this after a mirror write to re-run the native repaint. Returns
// false if unresolved. Game thread. See the impl for why this is value-/stat-/side-effect-
// neutral.
bool RefreshPointsHud();

}  // namespace ue_wrap::economy
