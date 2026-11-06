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

// What MakePawnInert did: Made (the pawn was written), NotAPawn (null or a non-pawn actor; nothing written),
// Unresolved (the Pawn class could not be looked up, so no actor could be judged; nothing written).
enum class PawnInertResult { Made, NotAPawn, Unresolved };

// A pawn begun by a deferred spawn and not yet finished is made inert: AutoPossessPlayer, AutoPossessAI and
// AutoReceiveInput zeroed, bBlockInput set, AIControllerClass nulled, so its BeginPlay takes no controller of
// either kind and no local input, and nothing that asks the class for a default controller gets one. It looks up
// the Pawn class and nothing else. Must run between the begin and the finish of the spawn. Game thread.
PawnInertResult MakePawnInert(void* deferredPawn);

// AActor::K2_DestroyActor. Game thread.
bool DestroyActor(void* actor);

}  // namespace ue_wrap::engine
