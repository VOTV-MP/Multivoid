// ue_wrap/kerfur.h -- engine substrate for the kerfurOmega NPC: save-key read,
// host-authoritative cosmetic state (command/spooky/face), thorough mirror parking
// (timer neutralize) and deterministic owned-child teardown. Pure reflection /
// UFunction access; NO net / gameplay logic (Principle 7).
//
// Ground truth: research/findings/kerfur/votv-kerfurOmega-coop-double-and-camera-RE-2026-06-14.md.
// The kerfur pet NPC is kerfurOmega_C (+ ~20 skin subclasses; the "base/less-intelligent"
// vs "upgraded" tiers are the same class's sentient/Type/upgrade flags, NOT a separate
// class -- so the kerfurOmega_C class gate + the subclass walk cover every tier/skin).
//
// The kerfur head-look / body-yaw pose reads live in ue_wrap/puppet.cpp (the per-tick pose
// stream); this file owns the lifecycle/state substrate the coop NPC-mirror path needs.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::kerfur {

// kerfurOmega_C-or-descendant gate (live-guarded). Resolves + caches kerfurOmega_C;
// false until the BP class loads. Every other entry point is class-gated through this.
bool IsKerfurActor(void* actor);

// True iff the NPC actor is a SAVE OBJECT -- it has a non-None int_save "Key" (FName
// property). GENERIC over any allowlisted NPC (not kerfur-gated). The EntitySpawn sender
// ships this as the `savePersisted` flag: a save object that the host's world holds is ALSO
// in the save the joining client booted, so the client has a LOCAL TWIN to ADOPT (class-
// match) rather than a duplicate to spawn. NOTE: only the PRESENCE of a key is portable --
// the VALUE is minted RANDOM per load (kerfurOmega::loadData drops the int_save key restore,
// bytecode-proven), so it differs across peers and is useless for matching. In 0.9.0n the
// kerfur is the only save-persisted pet NPC, but the check is class-agnostic.
bool HasSaveKey(void* actor);

// Host read of the kerfur's authoritative cosmetic/command state for the pose stream.
// Returns false (outs untouched) for non-kerfur actors. state = enum_kerfurCommand byte
// ("State"); spooky = the kill/spooky flag ("isSpooky"); face = faceMaterialIndex (clamped
// to a byte). Hot path (per-tick per-NPC) -> offsets are resolved ONCE + cached.
bool ReadKerfurState(void* actor, uint8_t& state, bool& spooky, uint8_t& face);

// Drive the host-authoritative command/spooky onto a PARKED mirror kerfur (plain field
// writes; the mirror runs no AI so nothing fights them -> the AnimBP state machine selects
// the right body animation). No-op on non-kerfur. Face material is intentionally NOT applied
// here (it needs the kerfur's own setFace UFunction; deferred -- see the findings doc).
void DriveKerfurState(void* actor, uint8_t state, bool spooky);

// THOROUGH-PARK companion to puppet::DisableCharacterTicks. DisableCharacterTicks stops the
// actor + CMC ticks, but NOT FTimerManager timers. A kerfur arms three looping timers in
// BeginPlay (timer_face 0.15s, timer_kerf 200s -> spooky-kill roller, checkDoor 0.5s) that
// keep running LOCAL AI on a parked mirror (the 200s roller can flip the mirror into a local
// kill state + re-point lookAt at the client camera). Clear all three via K2_ClearTimer (the
// exact inverse of the BP's K2_SetTimerDelegate arm). No-op on non-kerfur.
void NeutralizeAiTimers(void* actor);

// ---- Inc-3: host-authoritative menu command relay + ownership-aware follow ----------------

// Read the kerfur's `kill` (murderfur-mode) guard. actionName refuses the whole radial menu
// when kill==true; the relay replicates that guard before executing. false if unresolvable.
bool ReadKill(void* actor);

// Write ONLY the command State byte (enum_kerfurCommand) -- used to park a host-side
// owned-follow kerfur in idle (b1) so the BP's own move() stops re-pinning player-0, while we
// drive the path ourselves. No-op on non-kerfur. (DriveKerfurState writes State+spooky together
// for the mirror; this writes State alone on the host's real kerfur.)
void SetCommandState(void* actor, uint8_t state);

// Execute a radial-menu verb on the host's real kerfur exactly as the BP would: call
// kerfurOmega_C::actionName(playerActor, <zeroed Hit>, name) via ProcessEvent. The BP branch
// sets State + move()/dropObject() and honors its own kill/busy guards. `playerActor` must be a
// valid AmainPlayer_C (the non-follow verbs don't read it, but pass a live one). Returns false
// if actionName is unresolved. Game thread only.
bool RunActionName(void* kerfurActor, void* playerActor, const wchar_t* name);

// Drive an owned-follow kerfur toward `targetActor` (the requesting player's body -- the host
// mainPlayer_C or a remote puppet) via the engine's stock MoveTo async proxy
// (UAIBlueprintHelperLibrary::CreateMoveToProxyObject(Pawn=kerfur, Destination, TargetActor,
// AcceptanceRadius, false)). The kerfur is a real pawn with a working AIController (it follows
// player-0 natively), so this paths over navmesh. Pass BOTH targetActor AND its location as
// Destination (Destination is always honored; TargetActor following is best-effort for a
// non-pawn puppet). Fire-and-forget (the returned proxy self-manages). Game thread only.
void IssueFollowMoveTo(void* kerfurPawn, void* targetActor,
                       float destX, float destY, float destZ, float acceptRadius);

}  // namespace ue_wrap::kerfur
