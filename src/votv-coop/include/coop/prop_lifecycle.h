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
#include <vector>

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

// H2-redux 2026-05-28: snapshot-copies the maintained set of live
// keyed-interactable actors (Aprop_C lineage + chipPile/clump/trashBitsPile)
// into `out`. The set is seeded ONCE at Install() via a single
// GUObjectArray walk, then maintained by the Init POST observer (insert)
// and K2_DestroyActor PRE observer (evict). Replaces prop_snapshot's
// per-reconnect GUObjectArray walk (~150k objects -> ~2000 pointer copy).
//
// Best-effort, not transactional: a returned actor may have been
// destroyed in the window between snapshot-copy and the caller's use --
// callers MUST re-validate liveness (R::IsLive) before dereferencing.
// Returns count copied into `out` (`out` is cleared first).
size_t SnapshotKnownKeyedProps(std::vector<void*>& out);

// Tier 3 Props migration 2026-05-28: look up the host-allocated ElementId
// for a live AActor* tracked in the Prop Element shadow. Returns kInvalidId
// if the actor isn't tracked (e.g. unkeyed prop, or before the seed scan
// runs). Used by senders to populate `PropSpawnPayload.elementId` and
// `PropDestroyPayload.elementId` (v12 protocol).
coop::element::ElementId GetPropElementIdForActor(void* actor);

// Per-session state cleanup on disconnect. Returns counters for the
// log line. Clears: takeObj-in-flight flag, processed-Init dedupe set.
struct DisconnectStats {
    size_t initProcessedDropped = 0;
};
DisconnectStats OnDisconnect();

}  // namespace coop::prop_lifecycle
