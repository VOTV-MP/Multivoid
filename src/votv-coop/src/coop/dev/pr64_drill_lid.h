// coop/dev/pr64_drill_lid.h -- SRC-TREE PRIVATE: what the lid arm's two files share (pr64_drill_lid.cpp: the steps;
// pr64_drill_lid_aim.cpp: the box's readers and the aim at its slot). Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::dev::pr64_drill::detail {

// How long change B aims at the slot's own place before it sweeps offsets, counted from the first aim.
inline constexpr uint64_t kSlotDwellMs = 300;

// The step clock of the lid arm: true once `boundMs` have passed since the current step began.
bool Late(uint64_t boundMs);

// The box's `opened` bit, -1 when it does not read.
int ReadOpened(void* box);

// A TArray property of `obj`: its data and element count; false when it does not read.
bool ArrayOf(void* obj, const wchar_t* name, const uint8_t** data, int32_t* num);

// boxesTr[i], the slot's component; null when the array does not hold it.
void* SlotComp(void* box, int i);

// drives_in[0]: 1 a drive, 0 empty, -1 does not read.
int ReadDrive0(void* box);

// The element count of the box's array `name`, -1 when it does not read.
int32_t Count(void* box, const wchar_t* name);

// Aims at `target` and says whether the player's own interaction trace has `want` (the component when it is not null,
// else the actor `box`); false while it has not, and after the arm failed (the trace not taking it within kAimBoundMs
// of the step is a failure that names what it took).
bool Aimed(const ue_wrap::FVector& target, void* box, void* wantComp, const char* what);

// Says, before lid-ready, what slot 0's collider and the box's root are to the interaction trace (class, place, collision
// state, object type, response to the trace's channel, whether zipData is empty). The slot's line is kept for SlotAimed.
void LogSlotState(void* box);

// Change B's aim: aims at slot 0's component and says whether the player's interaction trace has it. When the trace has
// not taken it kSlotDwellMs after the first aim, the aim sweeps offsets from the slot's place, one per call, until one
// takes it; none taking it fails the arm with the slot's state and every component the sweep took.
bool SlotAimed(void* box, void* slot);

// Keeps the aim where SlotAimed found the slot (its place, plus the offset that took it).
void HoldSlotAim(void* box, void* slot);

void AimReset();

}  // namespace coop::dev::pr64_drill::detail
