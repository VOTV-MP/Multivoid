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

// The first live Kerfus, of any colour, that `accept` takes with `arg` (a null `accept` takes the first):
// default objects and dying or unreadable slots are passed over. Walks the object index by each exact class.
void* FindLive(bool (*accept)(void* obj, const void* arg) = nullptr, const void* arg = nullptr);

// `active` (on or off), `charging` (on its cord) and `energy`: false when this group did not resolve,
// the value left as it was.
bool ReadActive(void* k, bool& on);
bool WriteActive(void* k, bool on);
bool ReadCharging(void* k, bool& on);
bool WriteCharging(void* k, bool on);
bool ReadEnergy(void* k, float& energy);
bool WriteEnergy(void* k, float energy);

// Its navigation pawn (`pawn`): the Character its BeginPlay spawns 40 cm above it and welds onto its body, whose
// AI controller finds the path its wheel drives along. Null when not spawned, not live, or the field did not
// resolve.
void* NavPawn(void* k);

// Its wheel, an actor of its own (prop_kerfurWheel_C) that the `wheel` child-actor slot spawns at BeginPlay: the
// tick torques the wheel's body, and `physicsConstraint` holds it to the Kerfus's `wheelAttach`. False, with what
// was read, while any part is unset or dead, or the fields did not resolve.
struct Wheel {
    void* actor = nullptr;  // wheelObject
    void* body = nullptr;   // wheelComponent, the wheel actor's simulated mesh
    void* slot = nullptr;   // wheel, the ChildActorComponent on the Kerfus
};
bool ReadWheel(void* k, Wheel& out);

// The game's own refresh after `active` changes: upd(skipFace) -- the face, the sounds, the camera.
bool RunUpd(void* k, bool skipFace);

// Its E-press as `player` makes it: actionOptionIndex(player, action), action 8 the on/off. The verbs
// read no player, so a null one runs too. False when the call did not resolve or its body faulted.
bool RunActionOptionIndex(void* k, void* player, uint8_t action);

// The haunting's verb (kerfusPossessor_C calls it on every Kerfus): possess(at) turns it on, drops its
// task and drives it to `at`, where its own arrival test turns it off again.
bool RunPossess(void* k, const FVector& at);

}  // namespace ue_wrap::kerfus
