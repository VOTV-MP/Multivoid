// ue_wrap/actors/kerfus.h -- engine substrate for the plain kerfur (the Kerfus: p_kerfus_C and its
// colour variants): the fields its sync reads and writes, and its refresh, E-press and possess verbs.
// Pure reflection and UFunction access; no net or gameplay logic (principle 7). The variants inherit
// every field and function named here, so the group of fields resolves once, off the first Kerfus seen,
// and a field this build lacks is said once, naming it, and not looked up again. Every accessor takes a
// Kerfus (IsKerfus): once the group has resolved it reads whatever pointer it is handed. The kerfurOmega
// NPC is ue_wrap/actors/kerfur. Game thread, every function.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::kerfus {

// The family's base class, the name the script gate's class-scoped watches take.
constexpr const wchar_t* kClassName = L"p_kerfus_C";

// p_kerfus_C or a colour variant; false while the class is not loaded.
bool IsKerfus(void* obj);

// `active` (on or off), `charging` (on its cord) and `energy`: false when this group did not resolve,
// the value left as it was.
bool ReadActive(void* k, bool& on);
bool WriteActive(void* k, bool on);
bool ReadCharging(void* k, bool& on);
bool WriteCharging(void* k, bool on);
bool ReadEnergy(void* k, float& energy);
bool WriteEnergy(void* k, float energy);

// The game's own refresh after `active` changes: upd(skipFace) -- the face, the sounds, the camera.
bool RunUpd(void* k, bool skipFace);

// Its E-press as `player` makes it: actionOptionIndex(player, action), action 8 the on/off. The verbs
// read no player, so a null one runs too. False when the call did not resolve or its body faulted.
bool RunActionOptionIndex(void* k, void* player, uint8_t action);

// The haunting's verb (kerfusPossessor_C calls it on every Kerfus): possess(at) turns it on, drops its
// task and drives it to `at`, where its own arrival test turns it off again.
bool RunPossess(void* k, const FVector& at);

}  // namespace ue_wrap::kerfus
