// ue_wrap/world/upgrades.h -- the signal machine's eighteen upgrade levels
// (saveSlot.upgrades, an Fstruct_upgrades) and the laptop panel's own purchase arithmetic.
//
// The levels parametrise the download, ping, coordinate, refiner, radar and detector
// simulations. They live in ONE persistent struct on the save, so they are machine-wide rather
// than per-actor, and nothing in the game re-derives them: a write is the whole state change.
//
// Members are UserDefinedStruct fields, so each renders with a "_NN_GUID" tail that a recook
// re-mints; they resolve by PREFIX. The wire order is this file's table order, fixed here so a
// payload does not depend on the cooked declaration order. Principle 7: no coop or network
// logic -- coop/interactables/upgrade_sync owns the wire and the host-authoritative policy.

#pragma once

#include <cstdint>

namespace ue_wrap::upgrades {

// The struct's int members, and the length of every level array below.
inline constexpr int kLevelCount = 18;

// Read or write all levels at once, in wire order. False (nothing touched) while the store is
// unresolvable -- booting, or at the menu. Game thread.
bool ReadLevels(int32_t* out);        // out[kLevelCount]
bool WriteLevels(const int32_t* in);  // in[kLevelCount]

// One level, addressed by the laptop row's own `index` field. False if the index names no level
// row (a module row, or no row at all) or the store is unresolvable. Game thread.
bool ReadLevel(int panelIndex, int32_t* out);
bool WriteLevel(int panelIndex, int32_t value);

// True if `panelIndex` is one of the fifteen rows that buy a LEVEL. A module row is not one, and an index
// does not tell them apart: the module numbered 27 shares the level row's index.
bool IsLevelRow(int panelIndex);

// The two buttons of a row, by their cooked delegate-binding names: the row's own UFunctions, which nothing else in the
// game carries. PressModuleRow calls them; a drill that presses a row names them here and not by a second spelling. The
// script-gate watch rows in coop/interactables/upgrade_sync carry the same text as literals, since the fact-rows gate
// reads only literals.
inline constexpr const wchar_t* kBuyButtonFn =
    L"BndEvt__button_upgDownloadSpd_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
inline constexpr const wchar_t* kSellButtonFn =
    L"BndEvt__button_upgDown_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature";

// Presses a module row's own buy or sell button on this machine's live panel. False when no live module row of that
// index stands. Its body is the game's: index 27 writes the
// triangle level at the module's price, and the ten indices without a store (12-19, 22, 23: the row's getLevel and
// setLevel switches return a temporary) charge their price and hold no level; nine of them have a row on the laptop, as
// no widget carries 12 (ui_laptop's tree). The switch read; the 30 credits of row 23 measured on a client. Game thread.
bool PressModuleRow(int panelIndex, bool sell);

// The panel's own arithmetic, baked from the ui_laptop rows (the .cpp carries the table and the
// bytecode it was read from). Zero for a non-level index.
//   price  = price + accumulation * max(level - 1, 0)
//   refund = max(floor(price(level) * 0.75) - 1, 1)
int32_t PriceAtLevel(int panelIndex, int32_t level);

// The row's UNACCUMULATED price, which is what the buy button tests the balance against before it
// charges `PriceAtLevel`. The two differ from the second level up, and the game really does gate on
// this one: a row bought at a high level can take the balance below zero, since nothing clamps it.
// A host validating a client's purchase has to ask the same question the button asks, or a client
// is refused a purchase the host can make standing at the same panel.
int32_t BasePrice(int panelIndex);
int32_t RefundAtLevel(int panelIndex, int32_t level);
int32_t MaxLevel(int panelIndex);

// The gamemode's own apply hook, the second half of what the panel's button does: the button ends
// `upd(); gamemode->upgraded();`, and that second call is what re-derives the world from the new
// numbers -- the global sensorLevel material parameter, the object renderer's target, and every
// actor implementing int_upgrade_C, each told through upgraded_pcUpgrades. A level written without
// it is paid for and inert until something else runs it. It walks the actor list, so it belongs on
// a real change, never on a poll. False if the gamemode or the verb did not resolve. Game thread.
bool ApplyUpgradedHook();

// Re-run each open panel row's own upd(), so a level written from outside the UI shows on a
// panel that is already up. Cheap and safe when none is open: it walks live widgets only.
// Returns how many rows were refreshed. Game thread.
int RefreshOpenRows();

}  // namespace ue_wrap::upgrades
