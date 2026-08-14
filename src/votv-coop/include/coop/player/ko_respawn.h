// coop/player/ko_respawn.h -- KO RESPAWN: convert a lethal hit into a knock-out
// instead of the stock permadeath (death -> kick to the main menu -> rejoin).
//
// [death] section config: ko_respawn (on/off), ko_ragdoll_seconds,
// ko_invulnerable_seconds, ko_spawn_at_start.
//
// Two interception layers, both funneling into the same StartKO machine:
//   1. PRIMARY (clean): a ProcessEvent INTERCEPTOR on mainPlayer_C
//      "Add Player Damage" cancels a LETHAL hit (damage >= current health) BEFORE
//      the native death state ever enters -- no death screen, no native save-write.
//      Enemy hits (host-side enemy -> client puppet via the wire relay, or a direct
//      hit on the local pawn) all pass through this one funnel.
//   2. BACKSTOP (everything else): the net_pump death-policy detection (dead=true)
//      routes through HandleLocalDeath, which converts the death into a KO by
//      re-calling the native ragdollMode(mp, ragdoll, passOut, death=false) -- the
//      same function that set dead clears it. Covers fall / fire / radiation deaths
//      that bypass "Add Player Damage".
//
// The KO itself is the NATIVE faint state (ragdollMode with passOut=true) -- the
// same state the exhaustion faint / sleep use: the player flops (ragdoll), can't
// act, and is damage-immune for the whole KO (the interceptor cancels damage while
// active). After ko_ragdoll_seconds the player forceGetUp's, vitals restore to
// full, and (ko_spawn_at_start) the player teleports to the КПП start point.
// Held items and inventory are kept -- nothing is dropped.
//
// KNOWN TRADEOFF / VERIFY-ME (hands-on): a possessed ragdoll in the gameplay world
// was measured leaking ~165 MB/s in the DEATH flow (the reason the stock death
// policy flees to the menu). The KO holds that state for ko_ragdoll_seconds only;
// the leak figure for a pure faint was not measured separately -- if a 5 s KO
// ramps RSS on the local machine, lower ko_ragdoll_seconds.

#pragma once

namespace coop::net { class Session; }

namespace coop::ko_respawn {

// True when the [death].ko_respawn flag is on. Game thread.
bool Enabled();

// True while a KO is in progress (the player is down, respawn pending).
// Game thread. Used by net_pump to skip the permadeath flee on re-entry.
bool Active();

// The net_pump death-policy backstop: `mainPlayer` was detected dead (dead=true).
// Converts the death into a KO (native ragdollMode re-call with death=false +
// faint), returns true. Returns false when the feature is off (the caller runs
// the legacy permadeath flee). Idempotent: while a KO is already active it
// returns true without re-starting. Game thread.
bool HandleLocalDeath(coop::net::Session& session, void* mainPlayer);

// Set the session + register the AddPlayerDamage interceptor (idempotent; retried
// lazily until mainPlayer_C resolves). Called from subsystems::Install.
void Install(coop::net::Session* session);

// Game-thread per-tick drive: resolve-retry + the KO respawn timer.
// Called from subsystems::TickGameplay.
void Tick();

// Session-lifecycle resets (re-arm the timers, drop the cached player pointer).
void OnSessionStart();
void OnDisconnect();

}  // namespace coop::ko_respawn
