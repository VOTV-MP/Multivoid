// ue_wrap/actors/snack.h -- the snack family's own state: prop_snack_C's `uses` and the beer's `open`, read off the
// live actor and written back through the actor's own record, so the game's loadData sets what the state shows (the
// remaining pieces, the opened bottle's name). Engine-wrapper layer (principle 7): no network. Game thread.
//
// The family is prop_snack_C and every class below it by the class chain (candy, chips, milk, sushi, soda, and the
// beer with the wine under it); the beer family is prop_snack_beer_C and below.

#pragma once

#include <cstdint>

namespace ue_wrap::snack {

// Is `actor` a live prop_snack_C or a descendant of it? One class-chain walk.
bool IsSnack(void* actor);

// Is `actor` a live prop_snack_beer_C or a descendant of it (the wine)? The only class that carries `open`.
bool IsBeerFamily(void* actor);

// The snack's `uses` (int32) and, for a beer family member, its `open` (a bitfield bool); `open` is false for any
// other snack. False when `actor` is not a live snack or `uses` is unreadable.
bool ReadSnackState(void* actor, int32_t& uses, bool& open);

// Writes `uses` (and, for a beer family member, `open`) through the snack's own record: its most-derived getData is
// captured, the two values set in it, and the record handed back through ApplyRecord, which runs the game's loadData.
// `open` is ignored for a snack that has none. False when `actor` is not a live snack or the capture or the apply
// failed.
bool PatchSnackRecord(void* actor, int32_t uses, bool open);

}  // namespace ue_wrap::snack
