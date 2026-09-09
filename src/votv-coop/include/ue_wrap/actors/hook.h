// ue_wrap/actors/hook.h -- engine access and visual mirror management for VOTV
// grappling hooks and ropes (Ahook_C and derivatives).
// Principle-7 engine-wrapper layer (no network/coop state).
//
// In VOTV, Ahook_C is the base actor for hooks and ropes:
//   - hook_C (grappling hook)
//   - rope_C (rope)
//   - hook_Child_C / hook_flesh_C (variants)
//
// Each hook manages two ends:
//   - Head A (hook_single, UStaticMeshComponent)
//   - Head B (hook_single1, UStaticMeshComponent)
//   - Cable (UCableComponent) rendering the rope between Head A and Head B.
//
// RE: hook_classes.hpp from Dumper-7 SDK

#pragma once

#include "ue_wrap/core/types.h"
#include <cstdint>

namespace ue_wrap::hook {

// Snapshot of an Ahook_C instance's visual/geometric state.
struct HookSnapshot {
    bool valid = false;
    uint8_t classType = 0; // 0=hook_C, 1=rope_C, 2=hook_Child_C, 3=hook_flesh_C
    bool attachedA = false;
    bool attachedB = false;
    bool playerHooked = false;
    bool isThrown = false;
    float cableLength = 0.f;
    FVector posA{0.f, 0.f, 0.f};
    FRotator rotA{0.f, 0.f, 0.f};
    FVector posB{0.f, 0.f, 0.f};
    FRotator rotB{0.f, 0.f, 0.f};
};

// Resolve hook_C, rope_C, hook_Child_C, hook_flesh_C UClasses and property offsets.
// Idempotent; returns true once resolved.
bool EnsureResolved();

// True if obj's class derives from Ahook_C.
bool IsHook(void* obj);

// True if cls derives from Ahook_C.
bool IsHookClass(void* cls);

// Determine class type (0=hook_C, 1=rope_C, 2=hook_Child_C, 3=hook_flesh_C).
uint8_t ClassTypeOf(void* actor);

// Reads the current snapshot from a live Ahook_C actor. Returns true on success.
bool ReadHookSnapshot(void* hookActor, HookSnapshot& out);

// Spawns a visual mirror actor (physics, collision, and actor tick disabled).
void* SpawnHookMirror(uint8_t classType, const HookSnapshot& initial);

// Updates the mirror actor with the latest snapshot. If playerHooked and puppetActor is valid,
// aligns End B to the puppet's hand/view anchor.
void ApplyMirrorSnapshot(void* mirrorActor, const HookSnapshot& state, void* puppetActor);

// Destroys the mirror actor.
void DestroyHookMirror(void* mirrorActor);

}  // namespace ue_wrap::hook
