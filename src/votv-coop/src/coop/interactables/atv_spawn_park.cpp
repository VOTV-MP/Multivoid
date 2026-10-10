// coop/interactables/atv_spawn_park.cpp -- see coop/interactables/atv_spawn_park.h.
#include "atv_spawn_park.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"
#include <chrono>
#include <unordered_map>
#include <vector>

namespace coop::atv_sync::spawn_park {
namespace {
struct Parked {
    coop::net::AtvSpawnPayload payload{};
    uint32_t worldGen = 0;   // the world it arrived in; another world drops it
};
std::unordered_map<std::wstring, Parked> g_parked;
// The host runs a handful of runtime ATVs; a park past this describes no world of theirs.
constexpr size_t   kCap = 16;
constexpr uint64_t kPollMs = 500;
uint64_t g_nextPollMs = 0;
bool     g_saidCap = false;
uint32_t g_parkedEver = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

void Park(const std::wstring& key, const coop::net::AtvSpawnPayload& payload, const char* why) {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::Park");
    const uint32_t gen = ue_wrap::world_identity::Generation();
    auto it = g_parked.find(key);
    if (it == g_parked.end()) {
        if (g_parked.size() >= kCap) {
            if (!g_saidCap) {
                g_saidCap = true;
                UE_LOGW("atv: %zu runtime-ATV spawns already parked -- synthKey='%ls' (%s) dropped; the next "
                        "world-ready replay sends it again (said once)", kCap, key.c_str(), why);
            }
            return;
        }
        UE_LOGI("atv: runtime-ATV synthKey='%ls' parked until this world is ready (%s)", key.c_str(), why);
        g_parked.emplace(key, Parked{payload, gen});
        ++g_parkedEver;
        return;
    }
    it->second.payload = payload;   // the newest description of this ATV wins
    it->second.worldGen = gen;
}

void Discard(const std::wstring& key) { g_parked.erase(key); }

uint32_t ParkedEver() { return g_parkedEver; }
size_t Pending() { return g_parked.size(); }

void DrainReady() {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::DrainReady");
    if (g_parked.empty()) return;
    const uint64_t now = NowMs();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollMs;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    std::vector<coop::net::AtvSpawnPayload> ready;
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (it->second.worldGen != gen) {
            UE_LOGI("atv: a parked runtime-ATV spawn of a world since left dropped -- this world's replay sends it");
            it = g_parked.erase(it);
            continue;
        }
        ready.push_back(it->second.payload);
        ++it;
    }
    for (const auto& p : ready) OnAtvSpawn(p, 0);
}

void Clear() {
    g_parked.clear();
    g_nextPollMs = 0;
    g_saidCap = false;
}
}  // namespace coop::atv_sync::spawn_park
