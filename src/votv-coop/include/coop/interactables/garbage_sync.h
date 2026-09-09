// coop/interactables/garbage_sync.h -- coop replication of VOTV's garbage and trash entities.
//
// The family spans Aprop_C derivatives -- the garbage bag, bin, container and gun -- and actors
// that are not Aprop_C at all: trashBitsPile, garbageClump, chipPile. Pickup is per-class: a
// physics-handle grab for the bags and bins, and collect-and-morph for trashBitsPile, where the
// pile destroys itself and spawns N individual items at the player.
//
// Per-class state -- itemsInside, the amounts, the types -- is NOT broadcast, and that is the piece
// still unbuilt. It is fine right up until a client's blueprint walks a stale itemsInside array
// left by a per-peer-divergent spawner roll, dereferences a freed actor and crashes. What IS built
// are the two client-side guards Install() puts in: the container's blueprint body is cancelled,
// and the garbage spawners are cancelled, leaving the host's rolls the only ones.

#pragma once

namespace coop::net { class Session; }

namespace coop::garbage_sync {

// Install both client-side guards. Idempotent, and retried until the classes load, on independent
// latches so one success does not pre-empt the other's retry.
//
// The container guard PRE-intercepts Aprop_openContainer_C::ReceiveTick and checkPickup and cancels
// the blueprint body when the actor's class is prop_garbageContainer_C -- a pointer compare against
// the class resolved once at install, which allocates nothing on a tick path. Local pickup still
// works for the picker; the other peer just does not see the container's internal state update.
//
// The spawner guard cancels event_trashPiles' overlap, arirTrasher's trash and
// baseCleaner_trashBits' BeginPlay on a client, so the host's rolls are the only ones.
// tool_garbageSpawner is deliberately let through, being a per-shot player action, per principle 6.
// The underground spawner is NOT gated: it mints only dirthole_item_C, which is outside the sync
// universe, so suppressing it deleted the client's per-peer loot mounds with no host replacement --
// dirtholes are per-peer local by doctrine.
void Install();

// Session pointer carrier. Mirrors the prop_lifecycle::SetSession pattern
// so the interceptors can read the live role (host vs client) without
// touching engine state. Stored as atomic to be safe against the
// parallel-anim-worker ProcessEvent dispatch shape per
// ue_wrap/core/game_thread.h:118-120.
void SetSession(coop::net::Session* session);

}  // namespace coop::garbage_sync
