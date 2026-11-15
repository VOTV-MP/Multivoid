// coop/dev/cave_drill.cpp -- see coop/dev/cave_drill.h.

#include "coop/dev/cave_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>

namespace coop::dev::cave_drill {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kSpawnerPath = L"/Game/objects/antibreatherSPawner.antibreatherSPawner_C";
constexpr const wchar_t* kEggPath = L"/Game/objects/misc/wolfgangEggs.wolfgangEggs_C";

uint64_t g_clientSeenMs = 0;
bool     g_clientStood = false;
int      g_clientCounts = 0;
int      g_hostStep = 0;     // 0 before the stand and the call, 1 after
int      g_hostCounts = 0;
void*    g_standSpawner = nullptr;

void* LiveInstanceOf(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

size_t CountLive(const wchar_t* cls) {
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

void* Stand(const wchar_t* path, ue_wrap::FVector at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls ? ue_wrap::engine::SpawnActor(cls, at) : nullptr;
}

// A function of no arguments on `obj`; false when it did not resolve or did not run.
bool CallVerb(void* obj, const wchar_t* fnName) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && ue_wrap::Call(obj, f);
}

void SayCounts(const char* who, const std::string& name, const char* when) {
    UE_LOGI("[CAVE-DRILL] [%s] %s %s: %zu antibreather_C, %zu centipedeHead_C, %zu wolfgangJr_C, %zu centipedeSegmet_C, "
            "%zu centipedeSegmet_jr_C, %zu antibreatherSPawner_C, %zu wolfgangEggs_C, %zu eggyWolfgang_C, %zu parentWolfgang_C live",
            who, name.c_str(), when, CountLive(L"antibreather_C"), CountLive(L"centipedeHead_C"), CountLive(L"wolfgangJr_C"),
            CountLive(L"centipedeSegmet_C"), CountLive(L"centipedeSegmet_jr_C"), CountLive(L"antibreatherSPawner_C"),
            CountLive(L"wolfgangEggs_C"), CountLive(L"eggyWolfgang_C"), CountLive(L"parentWolfgang_C"));
}

}  // namespace

bool IsCaveDrill(const std::string& name) { return name == "caveWolfgang" || name == "caveEgg" || name == "caveHead"; }

void ClientStep(const std::string& name) {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    // 12 s in the session: the client's own spawner, at the world's origin, where nobody stands, and its own `cencipede`.
    if (!g_clientStood && since > 12000) {
        g_clientStood = true;
        SayCounts("C", name, "before the client's own spawner");
        void* spawner = Stand(kSpawnerPath, {0.f, 0.f, 0.f});
        const bool ran = CallVerb(spawner, L"cencipede");
        UE_LOGI("[CAVE-DRILL] [C] the client's own spawner %p stood at the origin; its cencipede called: %d", spawner, ran ? 1 : 0);
        SayCounts("C", name, "after the client's own cencipede");
    }
    if (g_clientCounts == 0 && since > 32000) { g_clientCounts = 1; SayCounts("C", name, "at 32 s"); }
    if (g_clientCounts == 1 && since > 52000) { g_clientCounts = 2; SayCounts("C", name, "at 52 s"); }
}

void HostStep(const std::string& name, uint64_t sinceFireMs) {
    if (g_hostStep == 0 && sinceFireMs >= 4000) {
        g_hostStep = 1;
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!player || !ue_wrap::engine::TryGetActorLocation(player, at)) {
            UE_LOGW("[CAVE-DRILL] [H] FAIL %s: no host player to stand the cave beside", name.c_str());
            return;
        }
        SayCounts("H", name, "before");
        if (name == "caveHead") {
            // Wolfgang stood by the engine's own spawn (the npc lane's interceptor), when the spawner's mint dies at birth.
            void* head = Stand(L"/Game/objects/centipedeHead.centipedeHead_C", {at.X + 700.f, at.Y, at.Z + 50.f});
            UE_LOGI("[CAVE-DRILL] [H] the head %p stood 7 m from the host's player, live %d", head, head && R::IsLive(head) ? 1 : 0);
        } else if (name == "caveWolfgang") {
            // The level's own spawner when the save has one (at the cave, on the ground its creatures walk), else one stood here.
            void* spawner = LiveInstanceOf(L"antibreatherSPawner_C");
            const bool stood = !spawner;
            if (stood) spawner = Stand(kSpawnerPath, {at.X + 700.f, at.Y, at.Z + 50.f});
            const bool ran = CallVerb(spawner, L"cencipede");
            // What the call made, which the spawner keeps in `brether`.
            const int32_t brOff = spawner ? R::FindPropertyOffset(R::ClassOf(spawner), L"brether") : -1;
            void* made = brOff >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(spawner) + brOff) : nullptr;
            UE_LOGI("[CAVE-DRILL] [H] the spawner %p stood 7 m from the host's player; its cencipede called: %d; brether %p '%ls' live %d",
                    spawner, ran ? 1 : 0, made, made ? R::ClassNameOf(made).c_str() : L"-", made && R::IsLive(made) ? 1 : 0);
            if (stood) g_standSpawner = spawner;  // ended before its own 10 s roll can make a creature of its own (below)
        } else {
            // 60 m from the host's player and from the spot the client joined at: out of any antibreather's sight.
            void* egg = Stand(kEggPath, {at.X, at.Y - 6000.f, at.Z + 50.f});
            void* fn = egg ? R::FindDispatchFunctionCached(R::ClassOf(egg), L"damageByPlayer") : nullptr;
            ue_wrap::ParamFrame f(fn);
            bool ran = false;
            if (fn && f.valid()) {
                f.Set<void*>(L"player", player);
                f.Set<float>(L"damage", 1.f);
                ran = ue_wrap::Call(egg, f);
            }
            UE_LOGI("[CAVE-DRILL] [H] the egg %p stood 60 m from the host's player; its damageByPlayer called: %d", egg, ran ? 1 : 0);
        }
        SayCounts("H", name, "right after");
    }
    if (g_standSpawner && sinceFireMs >= 12000) {
        CallVerb(g_standSpawner, L"K2_DestroyActor");
        g_standSpawner = nullptr;
    }
    if (g_hostStep == 1 && g_hostCounts == 0 && sinceFireMs >= 20000) { g_hostCounts = 1; SayCounts("H", name, "at 20 s"); }
    if (g_hostStep == 1 && g_hostCounts == 1 && sinceFireMs >= 40000) { g_hostCounts = 2; SayCounts("H", name, "at 40 s"); }
}

void OnDisconnect() {
    g_clientSeenMs = 0;
    g_clientStood = false;
    g_clientCounts = 0;
    g_hostStep = 0;
    g_hostCounts = 0;
    g_standSpawner = nullptr;
}

}  // namespace coop::dev::cave_drill
