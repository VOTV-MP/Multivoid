// coop/interactables/atv_spawn_park.cpp -- see coop/interactables/atv_spawn_park.h.
#include "atv_spawn_park.h"
#include "coop/interactables/atv_host_sync.h"   // UnadoptedDecidedForDrill
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"   // ApplyPendingSet, ResolveSpawn
#include "coop/net/protocol.h"
#include "coop/net/wire_key_util.h"        // WireKeyFromString
#include "ue_wrap/core/fname_utils.h"      // Resolved
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"
#include <chrono>
#include <unordered_map>
#include <vector>

namespace coop::atv_sync::spawn_park {
namespace {
struct Parked {
    coop::net::AtvSpawnPayload payload{};   // a spawn's description; a destroy carries the key alone
    uint32_t worldGen = 0;   // the world it arrived in; another world drops it
    bool     destroy = false;
    bool     drillHold = false;   // [dev] a spawn the ATV drill holds until the host's state for it is decided (HoldForDrill)
};
std::unordered_map<std::wstring, Parked> g_parked;
constexpr uint64_t kPollMs = 500;
uint64_t g_nextPollMs = 0;
bool     g_saidCap = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Say(const char* kind, const std::wstring& key, const char* why) {
    UE_LOGI("atv: an ATV %s waits -- %s (key='%ls')", kind, why, key.c_str());
}

// A new key past the cap is dropped, said once; it comes again only with a later replay of the host's set.
bool Full(const std::wstring& key, const char* kind, const char* why) {
    if (g_parked.size() < kCap) return false;
    if (!g_saidCap) {
        g_saidCap = true;
        UE_LOGW("atv: %zu ATV arrivals already parked -- the %s for key='%ls' (%s) dropped; it comes "
                "again only with a later replay of the host's set (said once)", kCap, kind, key.c_str(), why);
    }
    return true;
}
}  // namespace

void Park(const std::wstring& key, const coop::net::AtvSpawnPayload& payload, const char* why, bool say) {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::Park");
    const uint32_t gen = ue_wrap::world_identity::Generation();
    auto it = g_parked.find(key);
    if (it == g_parked.end()) {
        if (Full(key, "spawn", why)) return;
        Say("spawn", key, why);
        g_parked.emplace(key, Parked{payload, gen, false});
        return;
    }
    const bool wasDestroy = it->second.destroy;
    const bool held = it->second.drillHold && !wasDestroy;
    it->second = Parked{payload, gen, false, held};   // the newest description of this ATV wins
    if (wasDestroy || say) Say("spawn", key, why);
}

void ParkDestroy(const std::wstring& key, const char* why) {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::ParkDestroy");
    const uint32_t gen = ue_wrap::world_identity::Generation();
    auto it = g_parked.find(key);
    if (it == g_parked.end()) {
        if (Full(key, "destroy", why)) return;
        Say("destroy", key, why);
        g_parked.emplace(key, Parked{{}, gen, true});
        return;
    }
    const bool wasSpawn = !it->second.destroy;
    it->second = Parked{{}, gen, true};   // the host's last word on this key is the destroy
    if (wasSpawn) Say("destroy", key, why);
}

void HoldForDrill(const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::HoldForDrill");
    const auto it = g_parked.find(key);
    if (it != g_parked.end() && !it->second.destroy) it->second.drillHold = true;
}

void Discard(const std::wstring& key) { g_parked.erase(key); }

size_t Pending() { return g_parked.size(); }

void DrainReady() {
    UE_ASSERT_GAME_THREAD("atv_spawn_park::DrainReady");
    // The host's pending set is judged on every call, ahead of the early returns: it waits on the same
    // readiness as a parked arrival but is not one, and an empty pending set returns at once.
    ApplyPendingSet();
    if (g_parked.empty()) return;
    const uint64_t now = NowMs();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollMs;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    struct Ready { std::wstring key; coop::net::AtvSpawnPayload payload; bool destroy; };
    std::vector<Ready> ready;
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (it->second.worldGen != gen) {
            UE_LOGI("atv: a parked ATV %s of a world since left dropped -- this world's replay brings the host's set",
                    it->second.destroy ? "destroy" : "spawn");
            it = g_parked.erase(it);
            continue;
        }
        if (it->second.drillHold && !coop::atv_host_sync::UnadoptedDecidedForDrill(it->first)) {
            ++it;   // [dev] the drill's fixture: handed back once the host's state for it is kept or dropped
            continue;
        }
        ready.push_back(Ready{it->first, it->second.payload, it->second.destroy});
        ++it;
    }
    if (ready.empty()) return;
    // Asked once for the whole poll, not once per parked arrival, and on every poll with no miss held: a
    // library that resolves late is then found by the next poll. fname_utils::Resolved is lazy and retried
    // on each call, so before it resolves the cost is one probe per 500 ms poll.
    if (!ue_wrap::fname_utils::Resolved()) return;
    for (const auto& r : ready) {
        if (r.destroy) {
            coop::net::AtvDestroyPayload dp{};
            coop::net::WireKeyFromString(r.key, dp.key);
            OnAtvDestroy(dp);
        } else {
            ResolveSpawn(r.key, r.payload);
        }
    }
}

void Clear() {
    g_parked.clear();
    g_nextPollMs = 0;
    g_saidCap = false;
}
}  // namespace coop::atv_sync::spawn_park
