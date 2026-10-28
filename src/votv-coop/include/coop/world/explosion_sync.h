// coop/world/explosion_sync.h -- the explosions of the game, as a co-op peer sees them: a client's own game makes
// none (the host's are the shared world's), so the seam on every Blueprint deferred spawn dissolves the explosion a
// client's own game spawns, after it asks the host for the one its own player caused (coop/world/explosion_request.h).
// The host captures each explosion its game runs and sends it as an ExplosionSpawn; a
// client checks what it receives and spawns the copy in the lane's scope.
// A host or single player is untouched unless the dev birth line (ini explosion_birth_log) is on.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct ExplosionSpawnPayload;
}  // namespace coop::net

namespace coop::explosion_sync {

// Store the session and install the spawn seam (the client's dissolve) and the capture watch on explosion_C's
// begin-play (the host's send). Idempotent, any number of times; called from the load watchers, before the world is
// ready. An install that cannot complete is retried on a one-second throttle, at most 30 tries, then said once and
// left inert for the process; so is a capture watch the gate settles dead, said when it settles. It also installs the
// request lane (coop/world/explosion_request.h) with the same session, each call. Game thread.
void Install(coop::net::Session* s);

// Game thread, from TickGameplay: when the current world differs from the one last seen, the class tables (the
// explosion classes and the source classes) are reset and the request lane forgets its shots and queues; then the
// classes are resolved while any is unresolved (a lookup per unresolved class), and the request lane ticks.
void Tick();

// The class table and the counters are cleared, and the request lane's state with them.
void OnDisconnect();

// While one is open on this thread, a spawn of an explosion class is the lane's own and is not dissolved.
struct LaneSpawnScope {
    LaneSpawnScope();
    ~LaneSpawnScope();
    LaneSpawnScope(const LaneSpawnScope&)            = delete;
    LaneSpawnScope& operator=(const LaneSpawnScope&) = delete;
};
bool InLaneSpawnScope();

// Why a received explosion was dropped, one counter each.
enum class DropReason : uint8_t { Length, Class, IgnoreKind, NonFinite, Coord, Scale, Parking, Range, Flags, Count };

// Game-thread counters. `dissolved` counts the births the seam matched on a client; `dissolvedByName` those
// matched by class name before the table held the class. `lastDissolved` is the latest such actor and
// `lastDissolvedIndex` its object index: the pointer is only ever passed to reflection::IsLiveByIndex with
// that index. `destroyedByTask` counts the posted destroys that ran.
struct Counters {
    uint32_t dissolved, dissolvedByName, destroyedByTask, captured, sendFailed;
    uint32_t applied[3];
    uint32_t dropped[static_cast<int>(DropReason::Count)];
    void*    lastDissolved;
    int32_t  lastDissolvedIndex;
    uint64_t lastApplyMs;   // coop::net::NowMs() at the last applied blast (0 before the first); the log line's `t=`
};
Counters GetCounters();

// A host's ExplosionSpawn, as the dispatch hands it: checked in order (length, class, ignore kind, no flag bit past
// the two defined, every float finite, location in bounds, every scale strictly inside (0.01, 100), radius in
// [0, 1e5], shakes and fire values in [0, 1e6], force in [-1e8, 1e8], damage in [0, 1e30]); a failure is counted under
// its reason, logged once per reason, and the message dropped, never clamped. A good one posts Apply. Game thread.
// False = dropped.
bool OnSpawnReceived(const uint8_t* data, size_t len);

// Re-spawn the received explosion on this client, in the lane's scope. Runs only on a client in a session, never
// nested in another apply, and only once every parking watch is live (without its parkings a copy would write the
// host's world, so the apply fails closed). Game thread.
void Apply(const coop::net::ExplosionSpawnPayload& p);

}  // namespace coop::explosion_sync
