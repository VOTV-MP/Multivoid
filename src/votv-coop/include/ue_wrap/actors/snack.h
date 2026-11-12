// ue_wrap/actors/snack.h -- the snack family's own state: prop_snack_C's `uses` and the beer's `open`, read off the
// live actor and written back through the actor's own record, so the game's loadData sets what the state shows (the
// remaining pieces, the opened bottle's name). Engine-wrapper layer (principle 7): no network. Game thread.
//
// The family is prop_snack_C and every class below it by the class chain (candy, chips, milk, sushi, soda, and the
// beer with the wine under it); the beer family is prop_snack_beer_C and below.

#pragma once

#include "ue_wrap/core/reflection.h"   // FName
#include "ue_wrap/core/types.h"        // FVector, FRotator

#include <cstdint>
#include <vector>

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

// What a snack's last use reads off the snack to birth its wrapper: the `trash` name, the INSTANCE's `spawnAsObject`
// (not the class default) and the snack's whole transform, scale included.
struct WrapperFacts {
    reflection::FName trash{0, 0};
    bool spawnAsObject = false;
    FVector at{};
    FRotator rot{};
    FVector scale{1.f, 1.f, 1.f};
};
bool ReadWrapperFacts(void* snack, WrapperFacts& out);

// Begins a deferred spawn of `cls` at the snack's transform with the game's collision-handling byte (0 the class's own,
// 2 adjust where possible) and, before the finish, writes the actor's `foodData` (its bytes, when given) and `name`
// (when not NAME_None) as the script's SetStructure/SetNamePropertyByName do. Null when it could not begin.
void* BeginWrapperActor(void* cls, const WrapperFacts& at, uint8_t collision, reflection::FName nameBeforeFinish,
                        const std::vector<uint8_t>* foodData);

// Finishes the begun actor at the snack's transform; destroys it and returns false when the finish fails.
bool FinishWrapperActor(void* actor, const WrapperFacts& at);

// After the finish: writes the actor's `name` and runs its init(). False when either did not run.
bool StampNameAndInit(void* actor, reflection::FName name);


}  // namespace ue_wrap::snack
