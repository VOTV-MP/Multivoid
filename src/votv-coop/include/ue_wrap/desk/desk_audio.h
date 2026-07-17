// ue_wrap/desk_audio.h -- standalone engine access for the desk's unit-1
// AUDIO components (v115 desk sound-effect mirror). Principle-7
// engine-wrapper layer: NO network logic; coop::desk_snd_fx drives the
// forward/replay through here.
//
// The desk plays every unit-1 click/beep/loop through SIX audio components
// on AanalogDScreenTest_C (RE 2026-07-17, exhaustive structural opcode
// census over all 286 dumped assets): audio_coordKeyPress / audio_coordFail
// (one-shots with preset cues), audio_coordButtonSound / audio_coord_pingSound
// (SetSound+Play channels behind the playButtonSound/playPingSound helpers),
// corrds_loop / audio_coord_pingLoop (loops driven by SetActive/Activate).
// Every call site dispatches EX_VirtualFunction on a NATIVE target, so the
// dispatch funnels through UFunction->Func -- catchable by the Func-patch
// (docs/COOP_DISPATCH_VISIBILITY.md; K2_DestroyActor precedent), NEVER by
// the ProcessEvent detour.
//
// The component INDEX (0..5) is a wire contract (protocol.h DeskSndComp):
// kCompNames below is frozen in that order on both peers.

#pragma once

#include <cstdint>

namespace ue_wrap::desk_audio {

inline constexpr int kCompCount = 6;
inline constexpr int kFirstLoop = 4;  // indices 4/5 are the two loops

// Resolve the 6 component ObjectProperty offsets on the desk class, the
// AudioComponent Sound property, ActorComponent bIsActive, and the replay /
// hook-target UFunctions (AudioComponent:Play/SetSound, ActorComponent:
// SetActive, Activate). Piggybacks console_desk::EnsureResolved for the desk
// class + instance. Throttled lazy retry; idempotent; game thread.
bool EnsureResolved();

// The hook-target UFunction objects (null until EnsureResolved). These are
// ENGINE-class functions (AudioComponent/ActorComponent), resolvable before
// any desk instance exists.
void* PlayFn();
void* SetActiveFn();
void* ActivateFn();
void* DeactivateFn();  // L6: ActorComponent:Deactivate (the deck stop edge)

// L6 deck playback: is `comp` the desk's signalSound (unit-3 world-audible
// playback component)? NOT part of the 6-comp DeskSndFx wire table -- its
// edges route to coop::deck_play_sync instead. Same cache lifecycle +
// liveness discipline as IndexOfComp (one re-resolve owner); a pointer
// collision with the whitelist would be logged at refresh (never observed --
// 7 distinct ObjectProperties).
bool IsSignalSound(void* comp);

// L6 dev self-test: reflected Activate(bReset=true) / Deactivate on the
// desk's signalSound, dispatched UNGUARDED so the Func-patch seam sees an
// organic edge (proves patch -> routing -> classify -> wire pre-hands-on).
bool SelfTestSignalSound(bool on);

// Index of `comp` in the 6-comp whitelist of the CURRENT desk instance, or
// -1. Refreshes the cached pointers when the desk instance changed (level
// reload). O(6) pointer compares on the hot path -- safe deep inside the
// Func-patch callback (no engine calls when the cache is warm).
int IndexOfComp(void* comp);

// Read the comp's current Sound cue SHORT NAME (ASCII) into out (cap incl.
// NUL). False when unresolved / no sound / name longer than cap-1 (caller
// drops with a WARN -- never truncate a wire identity).
bool ReadCueName(void* comp, char* out, int cap);

// Ground-truth loop state (ActorComponent bIsActive) for the join re-assert.
bool ReadLoopActive(int compIdx, bool& outActive);

// Mirror replays (reflected dispatch -- runs ProcessEvent->Invoke->Func, so
// the caller MUST hold the coop-side wire-apply guard to kill the echo).
// ReplayPlay resolves the cue by short name (per-name cache; FindObject walk
// only on a cache miss) and does SetSound(cue)+Play(0) -- the exact native
// helper shape. ReplaySetActive(idx, on): ON -> SetActive(true,true) (all
// measured native ON sites use reset semantics), OFF -> SetActive(false,false).
bool ReplayPlay(int compIdx, const char* cueName);
bool ReplaySetActive(int compIdx, bool on);

// Session-end / level-change cleanup of the instance-pointer cache (class
// offsets and UFunctions persist -- they are engine/class-level).
void ResetCache();

}  // namespace ue_wrap::desk_audio
