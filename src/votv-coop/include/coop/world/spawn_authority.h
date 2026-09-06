#pragma once

// coop/world/spawn_authority.h -- the ONE owner of client-side shared-world
// spawner suppression.
//
// INVARIANT: a connected CLIENT ticks NO shared-world spawner and rolls NO
// shared-world spawn RNG; shared-world content arrives ONLY via the host wire
// (npc_sync / world_actor / the prop mirrors). Without it the deer, bp7 and
// hexahive spawners roll on a connected client, measured.
//
// ONE table, two tiers. t1 PARK parks the instance's tick through the engine's
// own SetActorTickEnabled (join-window pass, 1 Hz re-park of the cached set,
// 15 s reconcile walk for late instances) and RESTORES on session end, because
// a suppression is a loan. t3 CANCEL pre-cancels the spawner's entry fn, for
// ProcessEvent-dispatched functions only. Which class sits in which tier, and
// which are left running because their anchor is the LOCAL player, is the table
// in docs/npcs-and-kerfur.md, "Which spawners a client parks".

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::spawn_authority {

// Register the t3 PRE-cancel interceptors + resolve the t1 park classes (lazy
// ~1 Hz retry until all resolve, then latched). Stores `session` for the
// per-dispatch / per-tick gates. Game thread (net_pump InstallObservers ensure).
//
// A class is added to the table only once its product has a mirror: parking a
// spawner whose output nothing replaces just deletes the content. There is no
// t2 tier -- a shared driver-native caller filter, to starve a spawner that
// reaches its work through Delay or SetTimerDelegate -- because no class in the
// table needs one; the first Delay-latent family (beehive, mannequin) brings it,
// and mannequinSpawner also carries K2_DestroyActor reap duty, so it will need a
// (class, latent-UUID) grain rather than a class row. Discovery of new classes
// stays with the ini-gated rng_roll_census, not this module.
void Install(coop::net::Session* session);

// t1 park driver (TickGameplay, game thread): on an ACTIVE CLIENT session runs
// the initial park pass, the 1 Hz cached re-park, and the 15 s late-instance
// reconcile walk; on session end (any path) restores parked ticks. Cheap
// early-return when not an active client session and nothing is parked.
void Tick();

// Session-end restore (subsystems::DisconnectAll fanout). Re-enables tick on
// every still-live parked instance + clears the cache. Tick()'s gate also
// restores if the fanout is missed: a suppression is a loan, and the restore is
// the belt on the menu teardown's repayment.
void OnDisconnect();

// TRIPWIRE + late-instance signal. npc_sync's BeginDeferred interceptor is the
// wire-bypass and MIRROR seam; its client pass-through branch calls this, so a
// table spawner class that spawns on a connected client raises the alarm and
// names the late instance. Called from that branch (it fires on parallel-anim
// worker threads too -- this only compares
// pre-resolved class pointers + throttles a WARN; no walks, no engine calls).
// Returns true iff actorClass is a t1 park class (caller lets the spawn
// proceed; the reconcile walk parks the new instance within ~15 s).
bool NoteClientSpawnPassThrough(void* actorClass);

}  // namespace coop::spawn_authority
