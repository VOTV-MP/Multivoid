// ue_wrap/desk/dish_writers.h -- the two classes outside the dish that write its precision (dish.calibration) at a
// player's own verb. The toolgun's calibration tool: its init(toolgun) writes the tool's float parameter, unclamped,
// into the dish the toolgun's trace hit, the dish at the tool's index, or the dish of its name. The uncalibrator: its
// playerHandUse_LMB(player) zeroes the dish the player's hitResult names, the body's only effect. Principle-7
// engine-wrapper layer, no network logic: the names a watch registers, and a dev drill's calls.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::dish_writers {

inline constexpr const wchar_t* kToolClass = L"tool_setDishCalibration_C";
inline constexpr const wchar_t* kToolVerb = L"init";
inline constexpr const wchar_t* kUncalibratorClass = L"prop_uncalibrator_C";
inline constexpr const wchar_t* kUncalibratorVerb = L"playerHandUse_LMB";
inline constexpr const wchar_t* kToolgunClass = L"prop_toolgun_C";

// A new uncalibrator, calibration tool or toolgun at `at`, or null when its class is not loaded or the spawn fails.
// SpawnTool takes the tool's class (a tool_*_C: the calibration tool's kToolClass, or another tool's) so one entry
// serves every tool a drill runs.
// Game thread.
void* SpawnUncalibrator(const FVector& at);
void* SpawnTool(void* toolClass, const FVector& at);
void* SpawnToolgun(const FVector& at);

// The uncalibrator's use as the player's LMB runs it, on the dish `player`'s hitResult names. False when the verb
// does not resolve or the call fails. Game thread.
bool CallUncalibratorUse(void* uncalibrator, void* player);

// The tool's init as a toolgun's RMB runs it, on the dish at `index`: the tool set to `value` and `index` (and to the
// live gamemode if its spawn left it none), the toolgun's isRMB set. The tool destroys itself after the write. False
// when a field or the verb does not resolve, or the call fails. Game thread.
bool CallToolInitByIndex(void* tool, void* toolgun, int32_t index, float value);

// Any tool's init(toolgun), as a toolgun's press runs it: the tool reads what the toolgun's trace holds. False when
// the tool or the toolgun is not live, the tool's class has no init, or the call fails. Game thread.
bool CallToolInit(void* tool, void* toolgun);

}  // namespace ue_wrap::dish_writers
