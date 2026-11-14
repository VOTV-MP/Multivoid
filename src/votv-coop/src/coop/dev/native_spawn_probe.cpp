// coop/dev/native_spawn_probe.cpp -- see coop/dev/native_spawn_probe.h.

#include "coop/dev/native_spawn_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/npc_sync.h"
#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::native_spawn_probe {

UE_FACT_FN(kRowNb0Spawn, "dev.native_spawn_probe", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;

// One source seen: the names are FNames (an index compare per call, no render), rendered when a line prints.
// A client's key carries whether the ghost sweep had run, so a source seen on both sides of it has a line for each.
struct Key {
    R::FName src, out, fn;
    int32_t  offset;
    uint32_t count;
    bool     afterSweep;
};
constexpr int kMaxKeys = 64;

constexpr uint64_t kRetryMs = 500;
constexpr int      kTries = 10;

bool On() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::native_spawn_probe) == "record";
    return on;
}

coop::net::Session* g_session = nullptr;
bool     g_hookDone = false;
bool     g_hookFailed = false;  // latched for the process: a hook that failed kTries times does not heal
int      g_tries = 0;
uint64_t g_triedAt = 0;
void*    g_gsCdo = nullptr;  // an engine CDO: lives for the process
Key      g_keys[kMaxKeys] = {};
int      g_nKeys = 0;
bool     g_fullSaid = false;

uint64_t Now() { return ::GetTickCount64(); }

bool IsHost() { return g_session && g_session->role() == coop::net::Role::Host; }

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

std::wstring Render(const R::FName& n) { return n.ComparisonIndex ? R::ToString(n) : std::wstring(L"none"); }

// The key's line: its count when asked, and on a client whether the ghost sweep had run.
void SayKey(const char* verb, const Key& k, bool counted) {
    char tail[40];
    int  n = 0;
    tail[0] = 0;
    if (counted) n = std::snprintf(tail, sizeof tail, " count=%u", k.count);
    if (!IsHost()) std::snprintf(tail + n, sizeof tail - n, " after-sweep=%d", k.afterSweep ? 1 : 0);
    UE_LOGI("[NB-0] %s role=%s %ls from %ls fn=%ls@%d%s", verb, IsHost() ? "host" : "client", Render(k.out).c_str(),
            Render(k.src).c_str(), Render(k.fn).c_str(), k.offset, tail);
}

// The post hook: game thread, deep inside the engine spawn, so allocation-free until a new key prints.
// The game-thread guard is the one host_spawn_watcher's OnSpawnPost carries (coop/props/host_spawn_watcher.cpp):
// spawning is game-thread-only, so it never misses a spawn, and it keeps the plain statics below one
// thread's.
void OnSpawnPost(void*, void* source, void* result) {
    if (!GT::IsGameThread()) return;
    if (!g_session || !g_session->running()) return;  // no session: not a measurement
    if (!result || !source || source == g_gsCdo) return;  // a ProcessEvent-dispatched spawn: the interceptor's or ours
    void* const cls = R::ClassOf(result);
    void* const srcCls = R::ClassOf(source);
    if (!cls || !srcCls || !coop::npc_sync::IsAllowlistedClass(cls)) return;
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    const R::FName fn = frame.function ? R::NameOf(frame.function) : R::FName{0, 0};
    const bool sweep = !IsHost() && coop::npc_adoption::GhostSweepDone();
    const Key probe{R::NameOf(srcCls), R::NameOf(cls), fn, frame.codeOffset, 0, sweep};

    for (int i = 0; i < g_nKeys; ++i) {
        Key& k = g_keys[i];
        if (k.offset == probe.offset && k.afterSweep == probe.afterSweep && SameName(k.fn, probe.fn) &&
            SameName(k.src, probe.src) && SameName(k.out, probe.out)) {
            ++k.count;
            return;
        }
    }
    if (g_nKeys >= kMaxKeys) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[NB-0] record FULL at %d keys -- later new sources are not recorded", kMaxKeys);
        }
        return;
    }
    Key& k = g_keys[g_nKeys++];
    k = probe;
    k.count = 1;
    SayKey("spawn", k, /*counted=*/false);
    if (!IsHost()) UE_LOGI("[NB-0] NATIVE %ls -> %ls", Render(k.src).c_str(), Render(k.out).c_str());
}

void TryHook() {
    if (g_hookDone || g_hookFailed || Now() - g_triedAt < kRetryMs) return;
    g_triedAt = Now();
    const bool final = ++g_tries >= kTries;
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    const ue_wrap::facts::Resolved r = ue_wrap::facts::Resolve(kRowNb0Spawn);
    if (g_gsCdo && r && UH::InstallPostHook(r, &OnSpawnPost)) {
        g_hookDone = true;
        UE_LOGI("[NB-0] record live");
    } else if (final) {
        g_hookFailed = true;
        UE_LOGI("[NB-0] BROKEN hook BeginDeferredActorSpawnFromClass");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!On()) return;
    g_session = session;
    TryHook();
}

void OnDisconnect() {
    if (!On()) return;
    for (int i = 0; i < g_nKeys; ++i) SayKey("total", g_keys[i], /*counted=*/true);
    g_nKeys = 0;
    g_fullSaid = false;
    // Cleared until the next session's Install sets it; the record also returns while no session runs.
    g_session = nullptr;
}

}  // namespace coop::dev::native_spawn_probe
