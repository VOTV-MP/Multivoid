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

// True if `panelIndex` is one of the fifteen rows that buy a LEVEL. The module rows buy a
// one-shot unlock stored elsewhere and are not this lane's business.
bool IsLevelRow(int panelIndex);

// The panel's own arithmetic, baked from the ui_laptop rows (the .cpp carries the table and the
// bytecode it was read from). Zero for a non-level index.
//   price  = price + accumulation * max(level - 1, 0)
//   refund = max(floor(price(level) * 0.75) - 1, 1)
int32_t PriceAtLevel(int panelIndex, int32_t level);
int32_t RefundAtLevel(int panelIndex, int32_t level);
int32_t MaxLevel(int panelIndex);

// Re-run each open panel row's own upd(), so a level written from outside the UI shows on a
// panel that is already up. Cheap and safe when none is open: it walks live widgets only.
// Returns how many rows were refreshed. Game thread.
int RefreshOpenRows();

}  // namespace ue_wrap::upgrades
