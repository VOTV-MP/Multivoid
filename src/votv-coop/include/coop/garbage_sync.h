// coop/garbage_sync.h -- coop replication of VOTV's garbage/trash entities.
//
// Phase 5G (2026-05-27). Garbage/trash interactions in VOTV are a class
// family spanning Aprop_C derivatives (Aprop_garbageBag/Bin/Container/Gun)
// AND non-Aprop_C actors (AtrashBitsPile_C : Aactor_save_C;
// Aprop_garbageClump_C : AActor; AactorChipPile_C : AActor). Pickup is
// per-class -- PHC grab for the bags/bins, "collect-and-morph" for
// trashBitsPile (the pile destroys itself + spawns N individual Aprop_C
// items at the player). State (itemsInside, amountA/B, typeA/B, ...) is
// per-class and currently NOT synced -- which is fine until a client BP
// walks a stale `itemsInside` array from a per-peer-divergent
// undergroundGarbageSpawner roll, derefs a freed AActor*, and AVs. This
// module replicates the family incrementally:
//
//   Inc 1 -- this file. Stop the crash without designing new packets.
//            PRE-intercept Aprop_openContainer_C::ReceiveTick + checkPickup
//            on the CLIENT, gated to garbage subclasses (substring match
//            on "garbage" in class name), to skip the BP body that walks
//            the stale itemsInside. Local pickup still works for the
//            picker; the other peer just doesn't see the internal state
//            update of the container.
//   Inc 2 -- broadcast itemsInside diffs + non-Aprop_C trash entity state
//            (trashBitsPile, garbageClump, chipPile) via a new
//            ReliableKind::NonPropEntityState packet. Add Init POST hooks
//            on the non-Aprop_C trash classes (host-broadcast / client-
//            reproduce).
//   Inc 3 -- host-authoritative gating of the garbage spawners
//            (event_trashPiles, arirTrasher, baseCleaner_trashBits).
//            tool_garbageSpawner deliberately allow-through (per-shot
//            player action, principle 6). The undergroundGarbageSpawner
//            row was DELETED Fork C 2026-06-10: bytecode-proven to mint
//            only dirthole_item_C (outside the sync universe) -- the
//            suppression deleted the client's per-peer loot mounds with
//            no host replacement; dirtholes are per-peer LOCAL by doctrine.
//
// RE source: research/findings/votv-garbage-trash-interaction-RE-2026-05-27.md

#pragma once

namespace coop::net { class Session; }

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
