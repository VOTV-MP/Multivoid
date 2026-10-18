// ue_wrap/actors/sleep.h -- standalone engine access for VOTV's sleep and timelapse state: the gamemode's
// isSleep flag, the bed it sleeps in, the global time dilation and the nightmare probability override.
// Principle-7 wrapper, no network logic; coop/sleep_sync reads and writes through it.
//
// The whole timelapse is ONE engine call -- SetGlobalTimeDilation(20) at sleep entry, 1.0 at wake
// -- plus ONE world flag, mainGamemode.isSleep. The natural wake fires when saveSlot.sleep reaches
// 100, and that need REFILLS during sleep at the dilated rate. The nightmare roll runs every 500
// in-sleep seconds, weighted by bedSleepProb() -- the bed's own dreamProb, 0.15 with no bed, or
// mainGamemode.dreamProbability when that is >= 0, where -1 is the single-player sentinel and 0
// kills the rolls. A winning roll calls wakeup() and then createDream().
//
// gamemode.wakeup() is the idempotent END of the timelapse: re-possess, camera, dilation back to
// 1.0, isSleep false. It also rolls the 10% gearer gift, but only when saveSlot.sleep is >= 99 AND
// the bed is a bed_C. The sleep need itself is a vitals row (ue_wrap/actors/vitals, Field::Sleep).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; }

namespace ue_wrap::sleep {

// Resolve mainGamemode_C fields/verbs + the GameplayStatics dilation calls (throttled lazy
// retry). Game thread.
bool EnsureResolved();

// mainGamemode.isSleep -- THE world sleep flag (false if unresolved).
bool IsSleeping();

// The class name of the bed the gamemode sleeps in (mainGamemode.bed, set by sleep() before
// isSleep rises, null for a bedless entry; the non-ariral wakeup() path clears it, so read it
// only while asleep and keep it across a dream); empty with no bed or unresolved. Game thread.
std::wstring BedClassName();

// mainGamemode.origPawn: the pawn `sleep` took from the controller before it possessed the sleeping
// pawn, which is the player's body for the length of the rest. Null with no gamemode, unresolved or
// unset; the pointer is as live as the game keeps it, so a caller holding it across ticks wraps it in a
// CachedObjRef. Game thread.
void* OrigPawn();

// mainGamemode.dreamProbability: -1 = SP sentinel (use bed.dreamProb);
// 0 = nightmares suppressed. Returns false if unresolved.
bool SetDreamProbability(float v);

// UGameplayStatics::SetGlobalTimeDilation / GetGlobalTimeDilation via the
// CDO + a persistent WorldContextObject. Get returns <= 0 on failure.
bool SetGlobalTimeDilation(float v);
float GetGlobalTimeDilation();

// ---- dev-drill helpers (coop/dev/midnight_drill and coop/dev/sleep_drill) ----
// First live bed actor (bed_C or a descendant), or null. One GUObjectArray walk per call --
// one-shot use only.
void* FindBed();
// The bed actor (bed_C or a descendant) nearest `from`, or null; one pass over the object index per
// call, one-shot use only. Same descent test as FindBed.
void* NearestBed(const FVector& from);
// Reflected gamemode.sleep(bed, dropItem=false, ignoreRagdoll=true) -- the
// native entry incl. its validation gates (event/hunger/ragdoll aborts).
bool CallSleep(void* bed);

}  // namespace ue_wrap::sleep
