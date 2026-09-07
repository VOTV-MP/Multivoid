// ue_wrap/economy.h -- player credit balance (saveSlot.Points) accessor and AddPoints.
//
// Resolves AmainGamemode_C -> saveSlot, the canonical one-per-machine store, and reads and
// writes the int32 Points by reflected field NAME. Cooked offsets shift across recooks, so the
// gamemode pointer and the field offsets are resolved by name, cached, and revalidated through
// IsLive. Game-thread only (UObject access plus a UFunction dispatch). Principle 7: no coop or
// network logic -- coop/balance_sync owns the wire encoding and the host-authoritative policy.

#pragma once

#include <cstdint>

namespace ue_wrap::economy {

// Every EARNING and SPEND in the game goes through lib_C::addPoints(int32 Add, UObject
// __WorldContext). It adds to saveSlot.points, sets the player interface's points text, and
// adds to save_main.stats -- total_points for a gain, points_spent for a spend. mainGamemode's
// own addPoints is a forwarder into it, which is what AddPoints() below dispatches. (The one
// writer that bypasses it is saveSlot::reset_points, which zeroes the balance outright.)
//
// No hook can observe an earning as it happens: every blueprint call site of lib_C::addPoints
// dispatches EX_LocalVirtualFunction, below ProcessEvent. That is what rules out INTERCEPTING
// an earning -- vetoing or rewriting it at the moment it is credited -- so the shared balance
// is kept host-authoritative by polling and mirroring instead, in coop/balance_sync.

// The live UsaveSlot_C* (gamemode.saveSlot), or nullptr while unresolvable.
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
