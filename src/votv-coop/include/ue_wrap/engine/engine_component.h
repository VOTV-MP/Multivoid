// ue_wrap/engine/engine_component.h -- reading and writing an actor's components -- transforms,
// tick, visibility. Engine-wrapper layer (principle 7): each call marshals one UFunction call or
// one reflected field access, with no gameplay, network or coop state. Game thread unless a
// declaration says otherwise. Implementation: src/ue_wrap/engine/engine_component.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::engine {

// The world rotation of `actor`'s visible StaticMesh component, or the actor rotation if it owns
// none. A chipPile's visual roll lives on the component (its UserConstructionScript), not the root,
// so the host captures this and the bare proxy reproduces it with SetActorRotation. Game thread.
FRotator GetVisibleMeshWorldRotation(void* actor);

// The UCharacterMovementComponent subobject of a Character; nullptr if none.
void* GetCharacterMovementComponent(void* characterPawn);

// The UStaticMeshComponent subobject of `actor` (a reflection child walk); nullptr if none. Game
// thread.
void* GetStaticMeshComponent(void* actor);

// UActorComponent::SetComponentTickEnabled; the puppet's CMC is parked at spawn so it does not
// fight the pose drive. Game thread.
bool SetComponentTickEnabled(void* component, bool enabled);

// USceneComponent world location (K2_GetComponentLocation) and forward vector; (0,0,0) on failure.
FVector GetComponentLocation(void* component);

// USceneComponent::RelativeLocation, read raw at the sdk_profile.h offset: the Blueprint-authored
// value, stable once construction has run, where K2_GetComponentLocation is unsettled mid-init.
// (0,0,0) on null.
FVector GetComponentRelativeLocation(void* component);

// A UParticleSystemComponent's `Template`, read raw at a reflection-resolved offset; the cue sync
// matches a live PSC to its cue by it, because the EX_CallMath spawn that created it is invisible
// to the detour. nullptr if unresolved.
void* GetParticleSystemTemplate(void* particleSystemComponent);

// SetVisibility(visible, propagate) + SetHiddenInGame(!visible, propagate): shows a remote pawn's
// body meshes (an unpossessed pawn never unhides them) or hides the orphan's editor visualisers.
// Pass propagate=false to hide a component whose children must stay visible (ACharacter::Mesh can
// be the body mesh's AttachParent). Game thread.
bool SetComponentVisible(void* component, bool visible = true, bool propagate = true);

// Force VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones (a byte write; no setter
// exists): an unrendered remote body otherwise stops posing. Game thread.
bool SetAnimTickAlways(void* skeletalMeshComponent);

// USkinnedMeshComponent::SetSkeletalMesh(NewMesh, bReinitPose=true), resolved on the owning class.
// Game thread.
bool SetSkeletalMesh(void* skeletalMeshComponent, void* skeletalMeshAsset);

// USkeletalMeshComponent::SetAnimClass: assign and instantiate an AnimBP class. Game thread.
bool SetAnimClass(void* skeletalMeshComponent, void* animBlueprintClass);

// UPrimitiveComponent::CreateDynamicMaterialInstance: the slot's MID, parented to its current
// material; overriding a texture parameter re-skins the slot with no cooked material (the skins,
// docs/players.md). Game thread.
void* CreateDynamicMaterialInstance(void* component, int32_t elementIndex);

// UMaterialInstanceDynamic::SetTextureParameterValue; the kel body materials expose their diffuse
// as 'tex'. Game thread.
bool SetTextureParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, void* texture);

// UMaterialInstanceDynamic::SetScalarParameterValue. The name overload takes the parameter that
// is already an FName on the actor holding the material (grime's cleanParameter, which differs
// per subclass), so callers with one pass it through rather than re-interning a literal.
// Game thread.
bool SetScalarParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, float value);
bool SetScalarParameterValue(void* materialInstanceDynamic, const reflection::FName& param, float value);

// UStaticMeshComponent::SetStaticMesh, resolved on the owning class; recomputes bounds and
// collision. Rejects null: a caller that has failed to resolve a mesh and one that means "no mesh"
// pass the same argument, and blanking a component is the wrong answer to the first. Game thread.
bool SetStaticMesh(void* staticMeshComponent, void* staticMeshAsset);

// The same call with no mesh, which UE reads as "show nothing" -- the second of those two callers,
// saying so. Game thread.
bool ClearStaticMesh(void* staticMeshComponent);

// USceneComponent::SetMobility (0 Static, 1 Stationary, 2 Movable). A runtime-spawned
// AStaticMeshActor defaults to Static, on which SetStaticMesh and SetActorLocation are silent
// no-ops, so a moving proxy must be Movable first. Game thread.
bool SetComponentMobility(void* sceneComponent, uint8_t mobility);

// UPrimitiveComponent::SetMaterial; null reverts the slot to the mesh asset's default (the trash
// proxy clears a stale clump override that way). Game thread.
bool SetComponentMaterial(void* primitiveComponent, int32_t elementIndex, void* material);

// UStaticMesh::GetMaterial(index); null on failure. Game thread.
void* GetStaticMeshMaterial(void* staticMeshAsset, int32_t materialIndex);

// UActorComponent::K2_DestroyComponent (`contextObject` = the owning actor, for the engine's auth
// check). Game thread.
bool DestroyComponent(void* component, void* contextObject);

}  // namespace ue_wrap::engine
