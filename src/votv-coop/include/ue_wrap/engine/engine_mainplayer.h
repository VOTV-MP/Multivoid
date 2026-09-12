// ue_wrap/engine/engine_mainplayer.h -- the local player pawn's own state: its grab, its
// flashlight, its ragdoll mode and the damage it takes. Engine-wrapper layer (principle 7): each
// call marshals one UFunction call or one reflected field access, with no gameplay, network or
// coop state. Game thread unless a declaration says otherwise. Implementation:
// src/ue_wrap/engine/engine_mainplayer.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>
#include <cstdint>

namespace ue_wrap::engine {

bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor);

// True iff `localPlayer`'s grabbing_actor slot holds `actor`; no mutation. A held prop is skipped
// by the physics reconcile: forcing SimulatePhysics off mid-hold breaks the grab. Game thread.
bool IsMainPlayerGrabbing(void* localPlayer, void* actor);

// Eager-resolve the PHC.ReleaseComponent cache once the PhysicsHandleComponent class is loaded, so
// the first cross-peer PropDestroy does not fall through to the warn-and-clear fallback.
// Idempotent. Game thread.
bool WarmupPhcReleaseCache();

// UPhysicsHandleComponent::GrabbedComponent at the fixed offset sdk_profile.h names; nullptr if
// `phc` is null or dead or the slot is empty. Game thread.
void* ReadPhysicsHandleGrabbedComponent(void* phc);

// The AmainPlayer_C grab-state properties, read in one dispatch: grabbingActor and holdingActor
// cover the two carry paths (the physics handle vs the chipPile/clump carry). False on a null or
// dead pawn; `holdingActor` stays null when MainPlayer_holding_actor() is unresolved (a later
// recook added it). Game thread.
struct MainPlayerGrabState {
    void*  grabbingActor;  // AmainPlayer_C::grabbing_actor (PHC-held prop, or null)
    void*  holdingActor;   // AmainPlayer_C::holding_actor  (chipPile/clump morph carry, or null)
    bool   grabsHeavy;     // AmainPlayer_C::grabsHeavy     (PCC heavy-grab BP flag)
    bool   heavy;          // AmainPlayer_C::Heavy          (BP-side heavy state mirror)
    float  grabLen;        // AmainPlayer_C::grabLen        (Timeline current grab length)
};

bool ReadMainPlayerGrabState(void* mainPlayer, MainPlayerGrabState& out);

// AmainPlayer_C::lookAtActor, the interactable under the crosshair; nullptr if unresolved or
// nothing is aimed at. Reflection-resolved. Game thread.
void* ReadMainPlayerLookAtActor(void* mainPlayer);

// Direct write of lookAtActor, the interaction-trace result the BP re-derives each tick; lets a
// test aim the next InpActEvt_use dispatch at a chosen actor. Game thread.
bool WriteMainPlayerLookAtActor(void* mainPlayer, void* actor);

// The radial-menu confirm state, releaseEToUse and actionIndex, the index into getActionOptions.
// Both reflection-resolved. The kerfur radial verb is relayed from these because the actionName
// dispatch is internal to the Blueprint and invisible to ProcessEvent. Game thread.
bool ReadMainPlayerRadialSelect(void* mainPlayer, bool& releaseEToUse, int32_t& actionIndex);

// Write grabbing_actor and grabbing_component in one dispatch (the autotest's synthetic grab keeps
// the BP's "what am I holding" mirror in step); nullptr/nullptr clears. Game thread.
bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component);

// AmainPlayer_C component slots: GrabHandle (UPhysicsHandleComponent), HeavyGrabPCC
// (UPhysicsConstraintComponent), GrabTimeline (UTimelineComponent); nullptr on a dead pawn or an
// empty slot. Game thread.
void* ReadMainPlayerGrabHandle(void* mainPlayer);

void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer);

void* ReadMainPlayerGrabTimeline(void* mainPlayer);

// AmainPlayer_C::light_R, the flashlight USpotLightComponent; nullptr if the pawn or the slot is
// dead. Game thread.
void* GetMainPlayerLightR(void* mainPlayer);

// A flashlight light's authoritative state, for the wire payload.
struct FlashlightSnapshot {
    float intensity;
    float outerConeAngle;
    float innerConeAngle;
    bool  visible;
};

// Read the snapshot off a USpotLightComponent; false if `light` is null or dead. Game thread.
bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out);

// The AmainPlayer_C flashlight bools and mode byte the wire payload mirrors, read in one call. Game
// thread.
struct MainPlayerFlashlightState {
    bool flashlight;       // AmainPlayer_C::flashlight     (canonical on/off)
    bool hasFlashlight;    // AmainPlayer_C::hasFlashlight  (equipped guard)
    bool crankFlashlight;  // AmainPlayer_C::crankFlashlight (_c variant marker)
    uint8_t mode;          // AmainPlayer_C::flashlightMode (focused/spread enum)
};

bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out);

// Direct write of AmainPlayer_C::flashlight (the dev auto-toggle; the BP's input-guarded toggle is
// out of reach). Game thread.
bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState);

// ULightComponent::SetIntensity (marks the render state dirty). Game thread.
bool SetLightIntensity(void* light, float newIntensity);

// USceneComponent::SetVisibility alone, with no SetHiddenInGame: ULightComponent's override marks
// the render state dirty without forcing hidden-in-game onto the component. Game thread.
bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren);

// USpotLightComponent::SetOuterConeAngle / SetInnerConeAngle (both mark the render state dirty).
// Game thread.
bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle);

bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle);

// ---- Ragdoll / faint display state ----
// AmainPlayer_C drives every ragdoll cause through one UFunction and an AnimBP gate bool; offsets
// are name-resolved, UFunctions cached. Game thread.

// AmainPlayer_C::isRagdoll (the AnimBP gate) and ::dead; false, outs untouched, while unresolved.
bool ReadMainPlayerRagdollState(void* mainPlayer, bool& isRagdoll, bool& dead);

// AmainPlayer_C::ragdollMode(ragdoll, passOut, death); (true, true, false) is the faint pose, which
// works on an unpossessed puppet.
bool SetMainPlayerRagdollMode(void* mainPlayer, bool ragdoll, bool passOut, bool death);

// AmainPlayer_C::forceGetUp(): the get-up of a possessed player (its cleanup is tick-driven, so a
// puppet's flop ends by destroying its spawned body instead).
bool ForceMainPlayerGetUp(void* mainPlayer);

// AmainPlayer_C::forceWakeup(): the unconditional stand-up (the ubergraph block @25800 restores
// movement, capsule collision, the camera and input, reading no gate); the KO recovery's verb.
// Possessed players only.
bool ForceMainPlayerWakeup(void* mainPlayer);

// AmainPlayer_C::canRagdoll, ragdollMode()'s precondition: false early-outs every ragdoll cause.
// The Killer Wisp false-grab window forces it false on the host, since an HP pin cannot stop a
// ragdoll death. Resolved by property name, then a masked write. Game thread.
bool SetMainPlayerCanRagdoll(void* mainPlayer, bool allowed);

// Read the same bool back, so a test can assert the gate took.
bool ReadMainPlayerCanRagdoll(void* mainPlayer, bool& allowed);

// AmainPlayer_C::"Add Player Damage": the owning peer applies a host-relayed enemy hit on its own
// possessed pawn, so it runs through that peer's armour BP and drops saveSlot.health; early-outs on
// a puppet. `blood` is not cosmetic: it gates the bloodLoss effect (@2784), so a hit without it
// produces a death the game never produces. Game thread.
bool InvokeAddPlayerDamage(void* mainPlayer, float damage, bool blood = false);


// ---- The puppet's faint display ----
// A puppet never runs ragdollMode: its playerRagdoll_C's lifecycle assumes a possessed, ticking
// player, and on a tickless orphan the actor is never reaped and its PhysX keeps simulating. The
// display spawns the ragdoll body directly (SpawnPlayerRagdollBody), hides the puppet's meshes and
// pelvis-attaches the puppet (RagdollDisplay in coop/player/remote_player_ragdoll).

// ---- Damage body pulse (material swap) ----
// SavedMaterial (types.h) is (component, slot, original); the puppet renders two body meshes, the
// native ACharacter slot and mesh_playerVisible, so the saved set spans both. The caller owns the
// vector.
using ue_wrap::SavedMaterial;

// Swap both body meshes' materials to the hurt-flash material (a skeletal gore skin, so it renders
// red), caching the originals into `saved`; Restore puts them back. False, with no effect, when
// anything does not resolve. Game thread.
bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);

bool RestoreHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);

// Eager-resolve the hurt material and the material UFunctions once per puppet spawn. Game thread.
void WarmupHurtFlashCache();

// A loaded UMaterialInterface by object name; nullptr if not loaded. Game thread.
void* ResolveMaterialByName(const wchar_t* name);

}  // namespace ue_wrap::engine
