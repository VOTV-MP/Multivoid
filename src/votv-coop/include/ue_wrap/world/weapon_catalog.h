// ue_wrap/world/weapon_catalog.h -- what one swing of a held item can deal, read from the game's
// `list_weapons` UDataTable. The player's updateHold looks the row up by the hold slot's item name;
// its attack swings only when that row carries a montage and the attack flag, and deals the row's
// `damage`, times the row's `matEffDmg` entry when the struck material is one of its `matEff`
// (the player Blueprint's updateHold, the gate on its fire input, and its attack function).
// Engine-wrapper layer (principle 7): no network, no coop state, no policy; the host's door intent
// decides what the bound means. The walk is verified against the reflected `damage` column, and a
// disagreement invalidates the catalog, as the store's prices are. Game thread only.

#pragma once

#include <string>

namespace ue_wrap::weapon_catalog {

// One item's swing as the table defines it.
struct Swing {
    bool  canSwing  = false;  // a row with a montage and the attack flag: the fire input swings it
    float maxDamage = 0.f;    // its damage times its largest material multiplier, when one is above 1
};

// Builds the catalog if it is not built; false when it is unusable, and the caller refuses rather
// than guess. A table not yet loaded is retried at most every 3 s (its lookup walks every object).
bool Ready();

// The swing of the item named `itemName` (the hold slot's item name, which the game keys the row by).
// An item with no row cannot swing. False when the catalog is unusable.
bool Lookup(const std::wstring& itemName, Swing& out);

}  // namespace ue_wrap::weapon_catalog
