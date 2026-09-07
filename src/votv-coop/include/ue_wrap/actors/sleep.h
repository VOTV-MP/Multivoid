// ue_wrap/sleep.h -- standalone engine access for VOTV's sleep and timelapse state: the gamemode's
// isSleep flag, the global time dilation, the nightmare probability override and the saveSlot sleep
// need. Principle-7 wrapper, no network logic; coop/sleep_sync drives the sleep gate through it.
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
// the bed is a bed_C -- so a caller that grants rest must call wakeup() FIRST and write the need
// AFTER, or every mirror rolls its own gift.

#pragma once

#include <cstdint>

namespace ue_wrap::sleep {

// Resolve mainGamemode_C fields/verbs + the GameplayStatics dilation calls +
// the saveSlot sleep-need offset (throttled lazy retry). Game thread.
bool EnsureResolved();

// The live mainGamemode singleton (cached + liveness-checked). Null until
// the world is up.
void* Gamemode();

// mainGamemode.isSleep -- THE world sleep flag (false if unresolved).
bool IsSleeping();

// mainGamemode.dreamProbability: -1 = SP sentinel (use bed.dreamProb);
// 0 = nightmares suppressed. Returns false if unresolved.
bool SetDreamProbability(float v);

// Reflected gamemode.wakeup() -- the native timelapse END (idempotent: BP
// no-ops when !isSleep). Game thread.
bool CallWakeup();

// UGameplayStatics::SetGlobalTimeDilation / GetGlobalTimeDilation via the
// CDO + a persistent WorldContextObject. Get returns <= 0 on failure.
bool SetGlobalTimeDilation(float v);
float GetGlobalTimeDilation();

// saveSlot.sleep -- the sleep NEED (0..100; >= 100 triggers the natural
// wake check @151). Read returns false if unresolved.
bool ReadSleepNeed(float& out);
bool WriteSleepNeed(float v);

// ---- the WAITING-state camera ----------------------------------------------
//
// In single-player, sleep entry instantly retargets the view to the gamemode's sleepCam, the
// cinematic base shot. In the gate's WAITING state the lone sleeper must NOT see that view: the
// cinematic belongs to the ACCELERATE phase. These resolve the gamemode's own actors -- the camera
// and the body in bed -- so coop/sleep_sync can hold the waiting camera at the bed and hand it to
// the cinematic once the gate fills. All null-safe, since camera polish never blocks the gate.
void* SleepCam();
void* SleepingPawn();
// SetViewTargetWithBlend via the SLEEPING PAWN's controller -- during sleep
// the controller possesses the sleeping pawn (the local mainPlayer is
// unpossessed, so the usual local-player route is null mid-sleep).
bool SetSleepViewTarget(void* target);

// ---- dev-probe helpers (coop/dev/sleep_probe) ----
// First live bed actor (bed_C), or null. One GUObjectArray walk per call --
// probe/one-shot use only.
void* FindBed();
// Reflected gamemode.sleep(bed, dropItem=false, ignoreRagdoll=true) -- the
// native entry incl. its validation gates (event/hunger/ragdoll aborts).
bool CallSleep(void* bed);

}  // namespace ue_wrap::sleep
