// ue_wrap/actors/kerfur.h -- engine substrate for the kerfurOmega NPC: the save-key read,
// host-authoritative cosmetic state (command, spooky, face), thorough mirror parking (timer
// neutralise) and deterministic owned-child teardown. Pure reflection and UFunction access;
// no net or gameplay logic (principle 7).
//
// The pet is kerfurOmega_C plus about twenty skin subclasses. The base and upgraded tiers are
// the same class's sentient, Type and upgrade flags rather than separate classes, so the
// class gate plus the subclass walk covers every tier and skin.
//
// The head-look and body-yaw pose reads live in ue_wrap/actors/puppet.cpp, with the per-tick
// pose stream; this file owns the lifecycle and state substrate the NPC-mirror path needs.

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

// Drive the host-authoritative command and spooky flag onto a PARKED mirror kerfur, by plain
// field write: the mirror runs no AI, so nothing fights them and the AnimBP state machine
// picks the body animation. No-op on a non-kerfur. The face material is deliberately not
// applied here, since it needs the kerfur's own setFace UFunction.
void DriveKerfurState(void* actor, uint8_t state, bool spooky);

// THOROUGH-PARK companion to puppet::DisableCharacterTicks. DisableCharacterTicks stops the
// actor + CMC ticks, but NOT FTimerManager timers. A kerfur arms three looping timers in
// BeginPlay (timer_face 0.15s, timer_kerf 200s -> spooky-kill roller, checkDoor 0.5s) that
// keep running LOCAL AI on a parked mirror (the 200s roller can flip the mirror into a local
// kill state + re-point lookAt at the client camera). Clear all three via K2_ClearTimer (the
// exact inverse of the BP's K2_SetTimerDelegate arm). No-op on non-kerfur.
void NeutralizeAiTimers(void* actor);

// ---- host-authoritative menu command relay ---------------------------------------------------

// Read the kerfur's `kill` (murderfur-mode) guard. actionName refuses the whole radial menu
// when kill==true; the relay replicates that guard before executing. false if unresolvable.
bool ReadKill(void* actor);

// Execute a radial-menu verb on the host's real kerfur exactly as the BP would: call
// kerfurOmega_C::actionName(playerActor, <zeroed Hit>, name) via ProcessEvent. The BP branch
// sets State + move()/dropObject() and honors its own kill/busy guards. Only take_object and
// equipment read `playerActor` (holdObject_kerf, objectViewer assign); pass a live
// AmainPlayer_C all the same. Returns false if actionName is unresolved. Game thread only.
bool RunActionName(void* kerfurActor, void* playerActor, const wchar_t* name);

// Is `out` the variable the disc insert's player reads write: lib.getMainPlayer's out argument as
// passed from the Omega's ubergraph (`callerFunction`, whose frame is `callerLocals`)? The insert's
// four reads (get_reports, state 4) all write CallFunc_getMainPlayer_AsMain_Player_1; the Omega's
// other getMainPlayer reads -- murder mode, the petting, loadHoldItem (nothing calls it) -- write others.
// False until the class loads, and while this build's class lacks the variable (said once per class
// object). Game thread.
bool IsInsertPlayerRead(void* callerFunction, const uint8_t* callerLocals, const uint8_t* out);

// Whether the Omega holds a disc it took for its reports (`hasFloppy`). False when it is not an Omega or
// the field did not resolve. Game thread.
bool ReadHasFloppy(void* actor, bool& has);

// The Omega's `targetActor`, what its last move() went for: its follow sets the player it reads, fix_servers
// the server, a reports task the dish console; patrol and fix_transformers clear it, idle leaves it. Null
// when it is not an Omega, the field did not resolve, or it is unset. Game thread.
void* ReadTargetActor(void* actor);

}  // namespace ue_wrap::kerfur
