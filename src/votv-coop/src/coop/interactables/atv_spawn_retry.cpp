// coop/interactables/atv_spawn_retry.cpp -- bounded runtime-ATV spawn queue.
#include "atv_spawn_retry.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"
#include <chrono>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace coop::atv_sync::spawn_retry {
// A spawn that could not run yet (the wrapper unresolved, or the spawn itself failed) waits here and is retried, so a
// transient miss does not leave the ATV missing until a rejoin: the pose stream only moves an ATV that exists. A
// destroy cancels the wait; a spawn that lands, the budget running out, or the world leaving the stamped generation
// ends it.
struct PendingSpawn {
    coop::net::AtvSpawnPayload payload{};
    uint32_t tries = 0;
    uint64_t nextMs = 0;
    uint32_t worldGen = 0;
    bool worldBound = false;  // bind only when the receiving world is ready
};
std::unordered_map<std::wstring, PendingSpawn> g_pendingSpawn;
constexpr uint32_t kSpawnTries = 20;
constexpr uint64_t kSpawnRetryMs = 500;
constexpr size_t   kSpawnPendingCap = 16;

uint64_t SpawnNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void DeferSpawn(const std::wstring& key, const coop::net::AtvSpawnPayload& payload, const char* why,
                bool attempted) {
    auto it = g_pendingSpawn.find(key);
    if (it != g_pendingSpawn.end() && it->second.worldBound &&
        it->second.worldGen != ue_wrap::world_identity::Generation()) {
        g_pendingSpawn.erase(it);
        it = g_pendingSpawn.end();
    }
    if (it == g_pendingSpawn.end()) {
        if (g_pendingSpawn.size() >= kSpawnPendingCap) return;
        it = g_pendingSpawn.emplace(key, PendingSpawn{payload, 0, 0}).first;
    }
    it->second.payload = payload;   // the newest description of this ATV wins
    PendingSpawn& p = it->second;
    if (attempted && !p.worldBound) {
        p.worldGen = ue_wrap::world_identity::Generation();
        p.worldBound = true;
    }
    if (attempted && ++p.tries > kSpawnTries) {
        UE_LOGW("atv: runtime-ATV synthKey='%ls' still not spawned after %u tries (%s) -- given up", key.c_str(),
                kSpawnTries, why);
        g_pendingSpawn.erase(it);
        return;
    }
    p.nextMs = SpawnNowMs() + kSpawnRetryMs;
    if (attempted && p.tries == 1)
        UE_LOGW("atv: runtime-ATV synthKey='%ls' not spawned yet (%s) -- retrying", key.c_str(), why);
}

void RetryPendingSpawns() {
    if (g_pendingSpawn.empty()) return;
    // A row stamped for a world since left describes an ATV of that world: drop it; whatever
    // still exists comes back through the new world's own announce and connect snapshot.
    const uint32_t gen = ue_wrap::world_identity::Generation();
    for (auto it = g_pendingSpawn.begin(); it != g_pendingSpawn.end();)
        it = it->second.worldBound && it->second.worldGen != gen ? g_pendingSpawn.erase(it) : std::next(it);
    const uint64_t now = SpawnNowMs();
    std::vector<coop::net::AtvSpawnPayload> due;
    for (auto& kv : g_pendingSpawn)
        if (now >= kv.second.nextMs) due.push_back(kv.second.payload);
    for (const auto& p : due) OnAtvSpawn(p, 0);
}

void DiscardSpawn(const std::wstring& key) { g_pendingSpawn.erase(key); }
void Clear() { g_pendingSpawn.clear(); }
}  // namespace coop::atv_sync::spawn_retry
