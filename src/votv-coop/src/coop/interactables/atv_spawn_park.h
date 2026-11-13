// coop/interactables/atv_spawn_park.h -- an ATV's spawn or destroy, held until this client's world can take it.
//
// The host sends an ATV's spawn only to a world-ready slot and re-sends every ATV it holds at each world-ready
// announce, so an arrival in a world since left is answered by that replay. What the replay cannot answer is an
// arrival landing in THIS world while it is not ready for it -- a post-snapshot sweep re-arming the load probe, the ATV
// wrapper, the name library or the game's key map not resolved yet -- which would otherwise be dropped for good, the
// pose stream only moving an ATV that exists. Such an arrival is parked here, stamped with the world it arrived in,
// and handed back once the world reads ready; a world change drops it, the new world's replay bringing it again. A
// key holds one entry: the host's last word on it, a spawn (with its payload) or a destroy (the key alone). An
// arrival that fails once its world is ready is final: nothing here retries it. Game thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace coop::net { struct AtvSpawnPayload; }
namespace coop::atv_sync::spawn_park {
// The park holds at most one arrival per key of the host's set, so it takes the set's own cap (the most keys one set
// holds: atv_runtime_mirror.cpp bounds its set and record by this constant): a set the host sent whole must be
// parkable whole, or the keys past a smaller cap would be dropped while the edge still counts them. The client's
// other once-per-key sets (the refused keys) take the same bound.
inline constexpr size_t kCap = 256;
// Hold the newest spawn for this key until the world is ready (a destroy parked for the key is replaced); `why`
// names the readiness that was missing. The first park of a key says so once; `say` says it again for a key
// already held, which only a deliberate park wants.
void Park(const std::wstring& key, const coop::net::AtvSpawnPayload& payload, const char* why, bool say = false);
// Hold a destroy for this key until the world is ready: the host's last word, so a spawn parked for the key is
// dropped for it.
void ParkDestroy(const std::wstring& key, const char* why);
// [dev] The spawn parked for `key` is not handed back until atv_host_sync has decided the host's state for that key (kept for
// the copy's adoption, or dropped by a red), so the copy is adopted after the state. Only while the ATV drill's row is on.
void HoldForDrill(const std::wstring& key);
// An arrival that landed, was malformed, or was overtaken: nothing for this key waits any longer.
void Discard(const std::wstring& key);
// Tick, inside a gameplay world: apply the host's pending set when its world is ready, then hand each parked
// arrival back to ResolveSpawn / OnAtvDestroy, which park it again while the world is still not ready. A half-second
// poll of that readiness. The name library is asked once per poll: while it is unresolved every parked arrival
// stays parked without re-entering the handlers, and the miss is held until the poll after a world change.
void DrainReady();
void Clear();
// The arrivals parked now. For the ATV spawn drill.
size_t Pending();
}  // namespace coop::atv_sync::spawn_park
