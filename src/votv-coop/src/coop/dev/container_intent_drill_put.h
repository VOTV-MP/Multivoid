// coop/dev/container_intent_drill_put.h -- what the container intent drill's put arms share between their two files,
// container_intent_drill_put.cpp (the client, and what both halves read of an arm) and
// container_intent_drill_put_host.cpp (the host). A private header of coop/dev/container_intent_drill_put*.cpp (src
// tree, not include/).

#pragma once

#include "container_intent_drill_detail.h"

namespace coop::dev::container_intent_drill::detail {

// The lines the two peers say to each other on drill chat.
inline constexpr const char* kPutFixtureLine = "ci-put host fixture ";   // + "pack=<eid> item=<eid, 0 when none>"
inline constexpr const char* kPutDoneLine    = "ci-put client done ";    // + "ok=<0 failed|1 passed|2 not measured> stray=<n|-1>"

// The container the nested and empty arms pocket: a gembox, whose volume fits the player's pocket.
inline constexpr const wchar_t* kBoxPath  = L"/Game/objects/prop_container_gembox.prop_container_gembox_C";
inline constexpr const wchar_t* kBoxClass = L"prop_container_gembox_C";

// The arms in which no put may be asked: puthidden (the game only clears the hover) and putnested (the seat refuses).
inline bool PutIsNoPutArm(Arm a) { return a == Arm::PutHidden || a == Arm::PutNested; }

// The class the arm puts (null for puthidden), which the stray count looks for.
const wchar_t* PutItemClass(Arm a);

// The live prop elements of class `cls` within 500 cm of `target`, the target itself not counted; -1 when the target's
// place does not read. One snapshot per call: once per arm, never per tick.
int PutStrayCount(const wchar_t* cls, void* target);

// The host's half: its steps, and its state back to the start.
void PutHostReset();

// The reds of the put arms (container_intent_drill_put2.cpp): each dev row is read once and its knob set on the peer it
// breaks, every tick (a session's end puts the knobs back). Inert when no row is on.
void PutArmClientKnobs();
void PutArmHostKnobs();

}  // namespace coop::dev::container_intent_drill::detail
