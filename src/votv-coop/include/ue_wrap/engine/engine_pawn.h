// ue_wrap/engine/engine_pawn.h -- the controller half of a pawn: possession, control rotation, the
// view target and the camera. Engine-wrapper layer (principle 7): each call marshals one UFunction
// call or one reflected field access, with no gameplay, network or coop state. Game thread unless
// a declaration says otherwise. Implementation: src/ue_wrap/engine/engine_pawn.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::engine {

// APawn::GetController; nullptr if none.
void* GetController(void* pawn);

// AController::GetControlRotation: the mouse-look view rotation; (0,0,0) on failure. Game thread.
FRotator GetControlRotation(void* controller);

// Direct write of AController::ControlRotation, at the sdk_profile.h offset; the engine reads it
// next tick, and
void SetControlRotation(void* controller, const FRotator& rot);

// APlayerController::SetViewTargetWithBlend: repoint the view to `newViewTarget` over `blendTime`
// seconds. Game thread.
bool SetViewTargetWithBlend(void* playerController, void* newViewTarget, float blendTime);

// The view camera's world location and rotation (APlayerCameraManager); zero on failure. Game
// thread.
FVector GetCameraLocation();

FRotator GetCameraRotation();

// APawn::SpawnDefaultController: an AIController possesses the pawn, so its AnimBP poses, with no
// viewport, input or camera. Game thread.
bool SpawnDefaultController(void* pawn);

// AActor::K2_DestroyActor. Game thread.
bool DestroyActor(void* actor);

}  // namespace ue_wrap::engine
