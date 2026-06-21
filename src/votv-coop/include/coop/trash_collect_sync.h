// coop/trash_collect_sync.h -- mirror a freshly-spawned-and-grabbed item
// (the VOTV trash-pile collect).
//
// Pressing E on a trashBitsPile_C ("trash stack") spawns one Aprop_C trash
// item and auto-grabs it into the player's hands the same frame. The item is
// born with Key == None (its BP UCS hasn't minted a NewGuid yet), so our
// held-prop pose stream emitted unmatchable PropPose key='None' AND the
// Init-POST broadcast skipped it -> the held item never mirrored to peers.
//
// We CANNOT catch the collect via a UFunction observer: trashBitsPile_C::
// playerTryToCollect is dispatched BP->BP (ProcessInternal), which bypasses
// our ProcessEvent detour entirely (verified hands-on: a POST observer
// installed but NEVER fired while the item was clearly held). So detection
// happens where the engine state IS visible -- net_pump's held-prop send,
// which already resolves the grabbing_actor every tick.
//
// EnsureHeldItemBroadcast is called on the NEW-held edge from that send: if
// the held actor is a freshly-spawned UNKEYED Aprop_C, it force-mints a stable
// Key and broadcasts a PropSpawn so every peer spawns a mirror. From there the
// existing grabbing_actor pose stream carries the item into the collector's
// hands, and PropRelease/PropDestroy unwind it like any held prop.
//
// CRASH-SAFE for the non-Aprop_C garbageClump/chipPile (the actual trash): we
// DO broadcast them, but the receiver never runs physics on them. GetStaticMesh
// returns null for non-Aprop_C, so the clump mirror spawns physics-free and is
// driven KINEMATICALLY (per-tick SetActorLocation, no mesh) -- the inverse of
// the reverted-2a use-after-free, which resolved their real mesh and physics-
// drove a self-morphing actor. [[project-bug-trash-chippile-uaf-crash]]

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::trash_collect_sync {

// Install the pile-grab observer (a PRE observer on AmainPlayer_C::InpActEvt_use). Caches `session`
// (re-cached every call for reconnect) and registers the observer once mainPlayer_C is loaded
// (idempotent; retries on later calls until the class resolves). Call from the world-gated subsystem
// install. In docs/piles/08 this observer is PROBE-A only (a diagnostic): the host grab syncs via the
// host_spawn_watcher convert-spawn POST. Game thread.
void Install(coop::net::Session* session);

// Game thread. If `heldActor` is a live, UNKEYED (Key=None) Aprop_C, force-mint
// a stable Key on it and broadcast a PropSpawn under that Key (so peers spawn a
// mirror the held-pose stream can then drive into the collector's hands).
// Returns true iff it minted + broadcast. No-op (returns false) for: null/dead
// actors, non-Aprop_C (transient chip/clump -- crash safety), the host-authoritative
// garbageClump (docs/piles/08: trash_channel owns it via the source pile's eid -- never authored
// here), and already-keyed actors (a normal world-prop grab -- the peer already has it).
// Idempotent: once minted the Key is non-None, so a repeat call returns false.
bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* session);

// (docs/piles/08, 2026-06-21: the v81 MORPH -- here and in the retired coop/pile_morph -- is FULLY GONE,
// RULE 1+2. It re-skinned eid E in place but detected the land by a proximity FindNearestChipPile that
// false-fired on a NEIGHBOUR pile in a dense CLUSTER. Replaced by the host-authoritative trash channel:
// identity is the host eid end-to-end (POSITION IS NEVER IDENTITY), the clump<->pile link is the
// host_spawn_watcher convert-spawn POST (zero proximity), and a per-eid ctx drops stale stream packets.)

// (The mirror-PILE death-watch -- WatchPile / WatchPileAt / TickWatchReleasedPiles /
// NotifyPileConsumed -- was RETIRED 2026-06-17, RULE 1+2. It inferred "grabbed" from a watched
// pile's actor dying NEAR the local camera, which is unsound for a mobile physics actor: a peer
// repeatedly TOUCHING a pile eventually caused a near-camera non-grab death that was misread as a
// grab -> a spurious PropDestroy wiped the pile on both peers. Replaced by the InpActEvt_use PRE
// observer above (Install), which fires only on a real E-press grab. See trash_collect_sync.cpp +
// votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md.)

// Drop the cached session (full session teardown / aggregate disconnect). The trash entity ctx map is
// cleared separately by trash_channel::OnDisconnect.
void OnDisconnect();

}  // namespace coop::trash_collect_sync
