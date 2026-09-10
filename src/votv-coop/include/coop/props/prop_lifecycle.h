// coop/props/prop_lifecycle.h -- Aprop_C spawn/destroy/extract wire observers.
//
// The host-authoritative prop sync layer, three seams:
//   - Aprop_C::Init POST (subclass-aware GUObjectArray scan): the host broadcasts PropSpawn
//     for every BP-spawned prop derivative; the client suppresses intermediate-variant local
//     spawns (mushroom7_C) and waits for the mature variant on the wire.
//   - AActor::K2_DestroyActor PRE: bidirectional broadcast (food consumption, container
//     break). Echo-suppressed through the remote_prop incoming-destroy set.
//   - propInventory_C::takeObj PRE/POST: brackets a container extract so the nested Init POST
//     defers its broadcast (Key is a NewGuid until loadData runs) and takeObj POST broadcasts
//     with the restored saved UUID.
//
// Principle 7: gameplay/network logic, reaching the engine through reflection + game_thread.

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

// Host-authoritative intermediate-variant suppression predicate. Three
// call sites (Init POST host-broadcast, snapshot enumerate, client local
// spawn destroy) use this for symmetry. Public so prop_snapshot can
// share it.
bool IsWireSuppressedPropClass(const std::wstring& cls);

// PER-PLAYER state actors, NOT shared world props: each peer owns its own instance (per-save
// key, can never claim-bind). Never snapshot-expressed, never live-broadcast, never swept --
// but unlike IsWireSuppressedPropClass the LOCAL instance LIVES, because it is the player's
// own state and sweeping it as unclaimed fatals that peer at the next GC purge. Call sites:
// snapshot enumerate-skip, Init POST broadcast-skip, adoption-sweep universe-skip.
bool IsPerPlayerPropClass(const std::wstring& cls);

// Destroy a local prop via K2_DestroyActor, ECHO-SUPPRESSED: MarkIncomingDestroy
// runs before the call so our K2_DestroyActor PRE observer does not re-broadcast
// the destroy to peers. `deferred=true` schedules via game_thread::Post -- required
// when called from inside Aprop_C::Init POST (the engine is still executing
// FinishSpawningActor; the calling BP graph's continuation must complete first).
// `deferred=false` destroys immediately -- safe from plain game-thread context
// (e.g. the event_feed drain). Used by the client intermediate-variant suppression
// (Init POST) and the P2 connect-snapshot claim sweep
// (remote_prop_spawn deferred divergence sweep). Game-thread only.
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

// HOST kerfur conversion: register a BP-internally-spawned prop (the turn_off output -- the verb
// spawns it via EX_CallMath, so no Init POST fires for it) as a host Prop Element shadow WITHOUT
// the wire PropSpawn broadcast, since the sole conversion signal is KerfurConvert. Reads the
// actor's class + key, MarkPropElement's the shadow, and returns the host-range eid (kInvalidId
// on no-key / failure). The dying prop form is released the symmetric SILENT way through
// prop_element_tracker::UnmarkKnownKeyedProp (no PropDestroy). Game thread.
coop::element::ElementId RegisterHostPropSilent(void* actor);

// Per-session state cleanup on disconnect. Returns counters for the
// log line. Clears: takeObj-in-flight flag, processed-Init dedupe set.
struct DisconnectStats {
    size_t initProcessedDropped = 0;
};
DisconnectStats OnDisconnect();

}  // namespace coop::prop_lifecycle
