// coop/interactables/atv_spawn_park.h -- a runtime ATV's spawn, held until this client's world can take it.
//
// The host sends an ATV's spawn only to a world-ready slot and re-sends every runtime ATV at each world-ready
// announce, so a spawn arriving in a world since left is answered by that replay. What the replay cannot answer is a
// spawn landing in THIS world while it is not ready for it -- a post-snapshot sweep re-arming the load probe, the ATV
// wrapper not resolved yet -- which would otherwise be dropped for good, the pose stream only moving an ATV that
// exists. Such a spawn is parked here, stamped with the world it arrived in, and handed back once the world reads
// ready; a world change drops it, the new world's replay bringing it again. A spawn that fails once its world is
// ready is final: nothing here retries it. Game thread.
#pragma once
#include <string>
namespace coop::net { struct AtvSpawnPayload; }
namespace coop::atv_sync::spawn_park {
// Hold the newest spawn for this key until the world is ready; `why` names the readiness that was missing.
void Park(const std::wstring& key, const coop::net::AtvSpawnPayload& payload, const char* why);
// A spawn that landed, was malformed, or whose ATV was destroyed: nothing for it waits any longer.
void Discard(const std::wstring& key);
// Tick, inside a gameplay world: hand each parked spawn back to OnAtvSpawn, which parks it again while the world is
// still not ready. A half-second poll of that readiness.
void DrainReady();
void Clear();
}  // namespace coop::atv_sync::spawn_park
