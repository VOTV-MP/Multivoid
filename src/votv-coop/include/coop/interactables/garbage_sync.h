// coop/garbage_sync.h -- coop replication of VOTV's garbage and trash entities.
//
// The family spans Aprop_C derivatives -- the garbage bag, bin, container and gun -- and actors
// that are not Aprop_C at all: trashBitsPile, garbageClump, chipPile. Pickup is per-class: a
// physics-handle grab for the bags and bins, and collect-and-morph for trashBitsPile, where the
// pile destroys itself and spawns N individual items at the player. Per-class state (itemsInside,
// the amounts and types) is NOT synced, which is fine right up until a client's blueprint walks a
// stale itemsInside array left by a per-peer-divergent spawner roll, dereferences a freed actor and
// crashes.
//
// The module replicates the family incrementally, and increment 1 is what this file holds: stop the
// crash without designing new packets. PRE-intercept Aprop_openContainer_C::ReceiveTick and
// checkPickup on the CLIENT, gated to the garbage subclasses by a substring match on "garbage" in
// the class name, to skip the blueprint body that walks the stale array. Local pickup still works
// for the picker; the other peer simply does not see the container's internal state update.

#pragma once

namespace coop::net { class Session; }

// The increments after this one, for whoever picks the lane up:
//
//   2. Broadcast itemsInside diffs and the non-Aprop_C trash entity state (trashBitsPile,
//      garbageClump, chipPile) through a NonPropEntityState packet, with Init POST hooks on those
//      classes -- host broadcasts, client reproduces.
//   3. Host-authoritative gating of the garbage spawners (event_trashPiles, arirTrasher,
//      baseCleaner_trashBits). tool_garbageSpawner is deliberately let through, being a per-shot
//      player action, per principle 6. The underground spawner is NOT gated: it is bytecode-proven
//      to mint only dirthole_item_C, which is outside the sync universe, so suppressing it deleted
//      the client's per-peer loot mounds with no host replacement -- dirtholes are per-peer local
//      by doctrine.

namespace coop::garbage_sync {

// Install the client-side BP-body cancels for garbage open-containers.
// Idempotent. Safe to call before the BP classes are loaded -- internal
// resolution retries on the next call. Called from harness once the
// engine has booted.
void Install();

// Session pointer carrier. Mirrors the prop_lifecycle::SetSession pattern
// so the interceptors can read the live role (host vs client) without
// touching engine state. Stored as atomic to be safe against the
// parallel-anim-worker ProcessEvent dispatch shape per
// ue_wrap/game_thread.h:118-120.
void SetSession(coop::net::Session* session);

}  // namespace coop::garbage_sync
