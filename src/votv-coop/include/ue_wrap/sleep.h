// ue_wrap/sleep.h -- standalone engine access for VOTV's sleep/timelapse
// state (mainGamemode.isSleep + the global time dilation + the nightmare
// probability override + the saveSlot sleep need). Principle-7 engine-wrapper
// layer -- NO network logic; coop/sleep_sync drives the Minecraft-style sleep
// gate through here.
//
// RE (votv-sleep-nightmare-RE-2026-06-12.md): the whole timelapse is ONE
// engine call -- UGameplayStatics::SetGlobalTimeDilation(20) at sleep entry
// (@70543), 1.0 at wake (@68530) -- plus ONE world flag mainGamemode.isSleep
// @0x04EC. The natural wake fires when saveSlot.sleep >= 100 (the need
// REFILLS during sleep at the dilated rate). The nightmare roll (every 500
// in-sleep seconds) weights bed.dreamProb unless mainGamemode.
// dreamProbability @0x1030 >= 0 overrides it (-1 = the SP sentinel; 0 kills
// rolls). gamemode.wakeup() is the idempotent timelapse END (re-possess +
// camera + dilation 1.0 + isSleep=false); it also rolls the 10% gearer gift
// IFF saveSlot.sleep >= 99 at that moment -- callers that grant rest must
// call wakeup() FIRST and write the need AFTER, or every mirror rolls gifts.

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

// ---- the WAITING-state camera (v71.1, user directive 2026-06-13) ----
// In SP the sleep entry instantly retargets the view to mainGamemode.sleepCam
// (the cinematic base shot). In the gate's WAITING state the lone sleeper
// must NOT see the base view -- the cinematic belongs to the ACCELERATE
// phase. These resolve the gamemode's own actors so coop/sleep_sync can hold
// the waiting camera at the bed and hand it to the cinematic when the gate
// fills. All null-safe (camera polish never blocks the gate).
void* SleepCam();        // mainGamemode.sleepCam      @0x04F0 (ACameraActor)
void* SleepingPawn();    // mainGamemode.sleepingPawn  @0x1258 (the body in bed)
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
