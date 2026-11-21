// ue_wrap/devices/cargo_lift.h -- the cargo lift (cargoLift_C), the base's elevator. Its call buttons run its own
// actionOptionIndex with the use action: the press flips `dir` and the lift eases its `attach` point 500 cm up or back
// down over `alpha`, carrying its platform (the `lift` child actor), with `moving` set until it arrives; a press while
// it moves does nothing. Its identity across peers is its level-export FName (it is a placed actor with no save key).
// Engine-wrapper layer: each call marshals the lift's own fields and function, with no coop state. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::cargo_lift {

// The use action, the one the call buttons offer.
inline constexpr uint8_t kPressAction = 4;

// cargoLift_C and its fields resolved; false while the class is not loaded, asked again each call until it is.
bool EnsureResolved();

bool IsCargoLift(void* obj);

// The lift's level-export FName: deterministic across peers, as the garage's.
std::wstring GetNameKey(void* lift);

// `dir`: true once the lift was sent up, false once sent down (the state a press leaves, before the lift arrives).
bool ReadDir(void* lift, bool& up);

// Writes `dir`. The move loop reads it every frame, so a lift that moves turns at once: the reversal the Blueprint makes
// itself when its platform is blocked. True when the field was written.
bool WriteDir(void* lift, bool up);

// `moving`: the lift is between its stops; a press now does nothing.
bool ReadMoving(void* lift, bool& moving);

// The lift's own press: actionOptionIndex with the use action. True when the call ran.
bool Press(void* lift);

// The fields its move loop compares each frame (the Blueprint reverses the lift when its platform lags the point it eases
// by 10 cm or more, unless `ignoreCheck` is set), for a drill that measures a rider's effect on the platform. Each is
// resolved on its own, so a missing one fails only its reader and never the lift lane.

// `ignoreCheck`: the move loop's skip of that comparison, held for a second after each reversal.
bool ReadIgnoreCheck(void* lift, bool& ignore);

// The platform's primitive component (`comp`, the root of the `lift` child actor, set at begin play), or null.
void* PlatformComponent(void* lift);

// The platform's actor (`comp`'s owner), or null.
void* PlatformActor(void* lift);

// World Z of the `attach` sphere the move loop eases 0..500 cm up, and of the platform component. False when unread.
bool ReadAttachZ(void* lift, float& z);
bool ReadPlatformZ(void* lift, float& z);

}  // namespace ue_wrap::cargo_lift
