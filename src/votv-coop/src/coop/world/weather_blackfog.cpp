// coop/world/weather_blackfog.cpp -- the black fog event, host to clients; see coop/world/weather_blackfog.h.
//
// Bytecode ground truth (blackFog_C, mainGamemode_C::spawnBlackFog): the spawn verb returns while the
// gamemode's `blackfog` holds a live actor, else spawns one at the origin and stores it; the actor's
// ReceiveDestroyed clears that member and calls setEvent(false). The actor's `eyes` array holds the ghosts
// its 40 s timer spawned, destroyed when the 180 s hold ends.

#include "coop/world/weather_blackfog.h"

#include "coop/net/protocol.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <cstdint>

namespace coop::weather_blackfog {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

UE_FACT_FN(kRowSpawnBlackFog, "weather_blackfog", L"mainGamemode_C", L"spawnBlackFog", Declared);

// A member found once off its class: the layout is the same in every world, and a member missing from a
// loaded class does not appear later, so a failed pass latches OFF with one warning.
struct Member {
    const wchar_t* cls;   // for the log line
    const wchar_t* name;
    int32_t off = -1;
    bool failed = false;
};
Member g_gmFog{L"mainGamemode_C", L"blackfog"};  // the held blackFog_C
Member g_eyes{L"blackFog_C", L"eyes"};           // TArray<AActor*>, the ghosts it spawned

bool EnsureMember(Member& m, void* obj) {
    if (m.off >= 0) return true;
    if (m.failed || !obj) return false;
    m.off = R::FindPropertyOffset(R::ClassOf(obj), m.name);
    if (m.off >= 0) return true;
    m.failed = true;
    UE_LOGW("weather_blackfog: %ls.%ls did not resolve -- latched OFF; game version mismatch?", m.cls, m.name);
    return false;
}

std::atomic<bool> g_isClient{false};
std::atomic<bool> g_echo{false};  // ApplyFromHost is inside its spawnBlackFog call

// The world generation in which this episode's mirror spawn ran, 0 when none has. Cleared when the host
// reports clear, so a copy that ends on its own clock before the host's does is not spawned again, and
// compared to the current generation, so a reloaded world gets its own copy.
uint32_t g_spawnedGen = 0;
bool g_saidSpawnUnresolved = false;

// The gamemode's live blackFog_C, or null. False when the member does not read (no gamemode, no member).
bool ReadHeldFog(void*& gamemode, void*& fog) {
    fog = nullptr;
    gamemode = ue_wrap::world_singleton::Gamemode();
    if (!gamemode || !EnsureMember(g_gmFog, gamemode)) return false;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gamemode) + g_gmFog.off);
    fog = (p && R::IsLive(p)) ? p : nullptr;
    return true;
}

// Destroy the ghosts `fog` spawned, then `fog`. The array is read off the actor before it dies.
void EndFog(void* fog) {
    constexpr int32_t kMaxGhosts = 32;
    void* ghosts[kMaxGhosts] = {};
    int32_t n = 0;
    if (EnsureMember(g_eyes, fog)) {
        struct ArrayHead { void* const* data; int32_t num; };
        const auto& arr = *reinterpret_cast<const ArrayHead*>(static_cast<const uint8_t*>(fog) + g_eyes.off);
        if (arr.data && arr.num > 0 && arr.num <= 4096)
            for (int32_t i = 0; i < arr.num && n < kMaxGhosts; ++i)
                if (arr.data[i]) ghosts[n++] = arr.data[i];
    }
    int32_t destroyed = 0;
    for (int32_t i = 0; i < n; ++i)
        if (R::IsLive(ghosts[i]) && E::DestroyActor(ghosts[i])) ++destroyed;
    E::DestroyActor(fog);
    UE_LOGI("weather_blackfog: host fog CLEAR -> destroyed the client's black fog and %d ghost(s)", destroyed);
}

// The game's own spawnBlackFog on the gamemode, inside the echo bracket. True when the call ran.
bool SpawnMirror(void* gamemode) {
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::ResolveOn(kRowSpawnBlackFog, R::ClassOf(gamemode));
    if (!fn) {
        if (!g_saidSpawnUnresolved) {
            g_saidSpawnUnresolved = true;
            UE_LOGW("weather_blackfog: mainGamemode_C::spawnBlackFog did not resolve -- no client black fog");
        }
        return false;
    }
    ue_wrap::ParamFrame f(fn.Fn());
    if (!f.valid()) return false;
    g_echo.store(true, std::memory_order_release);
    const bool ran = ue_wrap::Call(gamemode, f);
    g_echo.store(false, std::memory_order_release);
    return ran;
}

}  // namespace

void Install(bool isHost) {
    g_isClient.store(!isHost, std::memory_order_release);
}

bool HostHasBlackFog() {
    void* gm = nullptr;
    void* fog = nullptr;
    return ReadHeldFog(gm, fog) && fog != nullptr;
}

void ApplyFromHost(const coop::net::WeatherStatePayload& payload) {
    if (!g_isClient.load(std::memory_order_acquire)) return;
    if (!GT::IsGameThread()) return;
    void* gm = nullptr;
    void* fog = nullptr;
    if (!ReadHeldFog(gm, fog)) return;  // no gamemode yet (loading) or no member: the next state asks again

    const bool hostActive = (payload.flags2 & coop::net::fog_flags2::kBlackFogActive) != 0;
    if (!hostActive) {
        g_spawnedGen = 0;
        if (fog) EndFog(fog);
        return;
    }
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (fog || g_spawnedGen == gen) return;  // a copy runs, or its episode already ran here
    if (!SpawnMirror(gm)) return;
    g_spawnedGen = gen;
    UE_LOGI("weather_blackfog: host BLACK FOG -> ran spawnBlackFog on the client (its own ghosts follow)");
}

bool MirrorEchoActive() {
    return g_echo.load(std::memory_order_acquire);
}

void OnDisconnect() {
    g_echo.store(false, std::memory_order_release);
    g_spawnedGen = 0;
}

}  // namespace coop::weather_blackfog
