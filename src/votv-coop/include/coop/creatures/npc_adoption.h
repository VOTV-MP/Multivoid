// coop/creatures/npc_adoption.h -- a joiner's deferred class-match adoption of save-persisted
// NPCs (the kerfur above all), and the ghost sweep that follows it.
//
// The game's own save load materialises a save-persisted NPC on a joining client where no hook
// fires and at an unpredictable moment, while the host also announces it as a save-persisted
// spawn; a mirror spawned for that announcement would leave the client with two. The stored key
// cannot pair them -- the kerfur's load overrides the base restore and every peer mints a random
// one -- so the portable identity is the CLASS, both peers having loaded the same save, and a
// deferred poll of GUObjectArray finds the twin and binds it. Adopting the real actor rather
// than parking a fresh one is also camera-safe: it is fully initialised before it is parked, so
// its destroy cascade tears down the `cam` child the way the host's does. An NPC that no
// announcement claims -- a kerfur turned OFF after the save, whose host world holds only the
// prop form -- is a genuine orphan, and the ghost sweep takes it once the snapshot is delivered
// and every adoption has converged. Game thread only, so no mutex: the pending table and the
// latches are touched from the net-pump tick and the reliable-dispatch path, both on it.

#pragma once

#include <cstdint>
#include <string>

namespace coop::npc_adoption {

// Record a pending adoption for a save-persisted EntitySpawn (called from npc_mirror::OnEntitySpawn
// when payload.savePersisted==1). Does NOT spawn anything -- Tick() polls for the local twin.
// `actorClass` is the client-resolved + allowlist-validated UClass of `classW`. Idempotent on eid
// (a connect re-announce updates the entry's pose + re-arms the timeout). No-op if eid is already
// bound as a mirror. Game thread.
void ArmAdoption(uint32_t eid, const std::wstring& classW, void* actorClass,
                 float locX, float locY, float locZ,
                 float rotPitch, float rotYaw, float rotRoll);

// Per net-pump tick driver (called from npc_mirror::TickClientNpcs). Two jobs, both NO-OP once
// converged (single integer compares -> zero steady-state cost):
//  (1) THROTTLED (5 Hz while any entry is pending) GUObjectArray scan that binds each pending
//      entry's local twin as a host mirror (class-match: live, allowlisted, untracked, non-CDO,
//      ClassOf==actorClass; nearest to the host pose when several twins exist), parks it host-
//      driven, and -- if no twin appears within the timeout -- fresh-spawns a mirror
//      (npc_mirror::SpawnFreshNpcMirror) so a host NPC is never permanently lost.
//  (2) ONE-SHOT ghost sweep: after the connect snapshot is delivered (OnSnapshotComplete) AND no
//      entries remain pending, fires npc_mirror::DestroyUntrackedClientNpcs() to remove genuine
//      orphans. Gated this way so it NEVER destroys a local twin before its adoption has a chance
//      to bind (the premature-sweep trap).
// Game thread only.
void Tick();

// The host finished delivering the connect snapshot (client-side SnapshotComplete in event_feed).
// ORDERING NOTE: the EntitySpawn handlers are game_thread::Post'd (deferred) while this runs INLINE
// in the same event-drain, so g_pending is EMPTY at this instant and the ArmAdoption tasks are
// still queued. Safety rests on FIFO game-thread ordering: those queued ArmAdoption tasks always
// drain before the NEXT net-pump Tick's ghost-sweep check, and the sweep is gated on g_pending
// being empty -- so the sweep waits out the just-armed adoptions. (This is WHY the sweep lives in
// Tick, never inline here -- see Tick.) Game thread.
//
// Runs for EVERY join incl. live-capture: the ghost sweep (Tick) reconciles the stale
// blob objects the host's live snapshot does not claim, gated on the same load-tail
// quiescence as adoption so a still-loading local twin is adopted, never swept.
void OnSnapshotComplete();

// A fresh connect replay is starting for this client (net_pump, right after it announces
// ClientWorldReady -- which the host answers with a fresh EntitySpawn replay + SnapshotComplete).
// Resets the per-world state: clears stale pending entries from a prior world and re-arms the
// snapshot-delivered + ghost-swept latches, so a save-transfer world swap re-adopts + re-sweeps the
// new world. Game thread.
void OnClientWorldReady();

// Net disconnect (all peers gone): wipe all per-session state.
void OnSessionEnd();

}  // namespace coop::npc_adoption
