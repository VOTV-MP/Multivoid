// coop/prop_lifecycle.h -- Aprop_C spawn/destroy/extract wire observers.
//
// The host-authoritative prop sync layer:
//   - Aprop_C::Init POST (subclass-aware GUObjectArray scan): host
//     broadcasts PropSpawn on every BP-spawned prop derivative; client
//     suppresses intermediate-variant local spawns (mushroom7_C) and
//     waits for the mature variant via the wire.
//   - AActor::K2_DestroyActor PRE: bidirectional broadcast (host AND
//     client destroys replicate -- food consumption, container break,
//     etc.). Echo-suppressed via the remote_prop incoming-destroy set.
//   - propInventory_C::takeObj PRE/POST: brackets container extracts so
//     the nested Init POST defers its broadcast (Key is NewGuid pre-
//     loadData) and takeObj POST broadcasts with the restored saved UUID.
//
// Principle 7: this is gameplay/network logic; talks to ue_wrap through
// reflection + engine + game_thread.

#pragma once

#include "coop/element/element.h"
#include "coop/net/protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::prop_lifecycle {

// Cache the session pointer. Call once at boot, BEFORE any code path
// that could fire DrainPending* or the observers (e.g. before NetPumpTick
// starts ticking). The Install / InstallInventory functions ALSO accept
// a session pointer for backward compatibility, but this dedicated
// setter exists so the session is bound from the moment Drain* may run
// (NetPumpTick can call Drain* on a frame BEFORE g_netLocal is resolved
// + Install* is reached, leaving the queue undrained until Install runs).
void SetSession(coop::net::Session* session);

// Install Aprop_C::Init POST + AActor::K2_DestroyActor PRE observers.
// First call does a one-shot GUObjectArray scan to find every Init
// UFunction in the prop_C lineage. Idempotent; retried each NetPumpTick
// until the prop_C base class is loaded.
void Install(coop::net::Session* session);

// Install propInventory_C::takeObj PRE/POST observers. Separate because
// the inventory class may load later than the prop class. Idempotent;
// retried each NetPumpTick until propInventory_C appears.
void InstallInventory(coop::net::Session* session);

// 2026-05-27: per-feature retry queues retired -- the reliable channel
// (coop/net/reliable_channel.cpp) now buffers internally so Send() always
// succeeds. EnqueuePropSpawnForRetry / DrainPendingPropSpawns / their
// PropDestroy twins went with them per RULE 2.

// Host-authoritative intermediate-variant suppression predicate. Three
// call sites (Init POST host-broadcast, snapshot enumerate, client local
// spawn destroy) use this for symmetry. Public so prop_snapshot can
// share it.
bool IsWireSuppressedPropClass(const std::wstring& cls);

// PER-PLAYER state actors, NOT shared world props (2026-06-10): each peer
// owns its own instance (per-save key, can never claim-bind). Never
// snapshot-expressed, never live-broadcast, never swept -- but unlike
// IsWireSuppressedPropClass the LOCAL instance lives (it is the player's
// own state; the 2026-06-10 smoke swept the client's
// prop_inventoryContainer_player_C as "unclaimed" and the client fataled
// at the next GC purge). Call sites: snapshot enumerate-skip, Init POST
// broadcast-skip, adoption-sweep universe-skip.
bool IsPerPlayerPropClass(const std::wstring& cls);

// Destroy a local prop via K2_DestroyActor, ECHO-SUPPRESSED: MarkIncomingDestroy
// runs before the call so our K2_DestroyActor PRE observer does not re-broadcast
// the destroy to peers. `deferred=true` schedules via game_thread::Post -- required
// when called from inside Aprop_C::Init POST (the engine is still executing
// FinishSpawningActor; the calling BP graph's continuation must complete first).
// `deferred=false` destroys immediately -- safe from plain game-thread context
// (e.g. the event_feed drain). Used by the client intermediate-variant suppression
// (Init POST) and the P2 connect-snapshot claim sweep
// (remote_prop_spawn::DestroyUnclaimedDivergentProps). Game-thread only.
void DestroyLocalProp(void* actor, bool deferred);

// Express a freshly-spawned KEYED prop on the wire -- the SAME canonical keyed
// broadcast as the Aprop_C::Init POST observer (filters + HasProcessedInit
// dedupe + keyed PropSpawn payload + send + self-claim), callable from a
// DIFFERENT spawn seam. For props whose own init() is BP-internal
// (EX_LocalVirtualFunction from the UCS) and so NEVER fires the Init-POST
// observer -- the sandbox Q-menu / toolgun spawns (host_spawn_watcher hooks
// FinishSpawningActor POST, where the Key is already minted, and calls this).
// Idempotent vs the Init-POST path (shared HasProcessedInit latch). GAME-THREAD
// ONLY (runs ProcessEvent reads on the actor).
void ExpressSpawnedProp(void* actor);

// Explicit destroy-sync for a TRACKED prop whose K2_DestroyActor our PRE
// observer could not see (BP-internal by-name call -- prop_kerfurOmega_C::
// spawnKerfuro's self-destroy, v67). Broadcasts PropDestroy built from the
// Prop ELEMENT's stored key (never dereferences `actorKey` -- safe on a
// PendingKill or GC-purged pointer; the pointer is only a map key for the
// tracker teardown) then drains the element via the same UnmarkKnownKeyedProp
// path the organic destroy takes. No-ops when the element is already gone
// (double call / raced the real PRE observer). Game thread.
void SyncDestroyedTrackedProp(void* actorKey, coop::element::ElementId eid);

// GetPropElementIdForActor moved to coop::prop_element_tracker (M-1
// 2026-05-29 follow-up). #include "coop/prop_element_tracker.h" instead.

// Per-session state cleanup on disconnect. Returns counters for the
// log line. Clears: takeObj-in-flight flag, processed-Init dedupe set.
struct DisconnectStats {
    size_t initProcessedDropped = 0;
};
DisconnectStats OnDisconnect();

}  // namespace coop::prop_lifecycle
