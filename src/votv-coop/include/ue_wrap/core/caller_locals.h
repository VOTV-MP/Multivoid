// ue_wrap/core/caller_locals.h -- edit a Blueprint caller's locals in place, from a seam that runs while the
// caller's call is open (a native pre hook, a script-gate callback), where the caller's frame is reachable
// by ufunction_hook::CurrentCallerFrame or script_gate::Call::callerLocals. A local of the frame sits at its
// Offset_Internal (reflection::FindPropertyOffset on the calling function). Two shapes of local, each
// edited where it stands and with no allocation: a TArray<AActor*> loses elements, and an FHitResult loses
// its hit actor and component, its location kept. Engine-wrapper layer (principle 7): no gameplay or
// network logic; which element or hit to drop is the caller's predicate.

#pragma once

#include <cstdint>

namespace ue_wrap::caller_locals {

// A cleared TWeakObjectPtr: {ObjectIndex -1, ObjectSerialNumber 0}, which resolves to no object.
constexpr uint64_t kNullWeakObjectPtr = 0x00000000FFFFFFFFull;

// Remove from the TArray<AActor*> at `locals + arrayOffset` every element `drop(ctx, actor)` returns true
// for, keeping the order of the rest, and shrink its count; the buffer is the caller's and is neither
// reallocated nor freed. Returns the count dropped. Refuses, returning -1 and logging once, a null `locals`
// or `drop`, a negative offset, a count outside [0, max] or a null data pointer with a positive count.
// `remaining`, when given, receives the array's count after the removal; on a refusal it is not written.
int RemoveActors(uint8_t* locals, int32_t arrayOffset, bool (*drop)(void* ctx, void* actor), void* ctx,
                 int* remaining = nullptr);

// Where FHitResult keeps its hit actor and component (TWeakObjectPtr members) and how large it is, by the
// engine struct's own reflection.
struct HitLayout {
    int32_t actor = -1;
    int32_t component = -1;
    int32_t size = 0;
};

// Fill `layout` from the engine's HitResult struct; false, and the fields as they resolved, when the struct
// or a member is not found or a member does not fit the size. Walks the object array for the struct:
// resolve once at install, not per dispatch. Game thread.
bool ResolveHitLayout(HitLayout& layout);

// Write kNullWeakObjectPtr over the hit actor and component of the FHitResult at `hit`, and nothing else.
// Refuses a null `hit` and a layout whose members do not lie inside its size.
bool ClearHitActor(uint8_t* hit, const HitLayout& layout);

// A boot check of the two edits on buffers it builds with synthetic offsets, so it needs no engine. Logs
// its result and keeps it for SelftestPassed.
bool RunSelftest();
bool SelftestPassed();

}  // namespace ue_wrap::caller_locals
