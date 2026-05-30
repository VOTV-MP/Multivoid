// coop/dev/spawn_npc.cpp -- see coop/dev/spawn_npc.h.

#include "coop/dev/spawn_npc.h"

#include "coop/ini_config.h"
#include "coop/npc_sync.h"
#include "coop/players_registry.h"
#include "coop/shutdown.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

namespace coop::dev::spawn_npc {

namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

namespace {

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Spawn an allowlisted NPC ~2.5 m in front of the local player, facing them, via
// BeginDeferredActorSpawnFromClass + FinishSpawningActor. The spawn goes through
// the very UFunction npc_sync's interceptor hooks, so on the HOST the interceptor
// runs the full sync path (AllocAndInstall an Npc Element + broadcast EntitySpawn
// + let the spawn proceed) and connected clients materialize a mirror. This is the
// ONLY programmatic NPC-spawn trigger (VOTV NPCs spawn only from purchase/scripted
// events). Game thread only. (Extracted from npc_sync.cpp 2026-05-30 to keep it
// under the 800-LOC soft cap; the spawn refs come from npc_sync::GetDevSpawnRefs.)
void SpawnNpcInFront(const wchar_t* className) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    coop::npc_sync::DevSpawnRefs refs;
    if (!coop::npc_sync::GetDevSpawnRefs(refs)) {
        UE_LOGW("spawn_npc: NPC spawn refs not ready (Install not complete?) -- ignoring");
        return;
    }
    void* actorClass = R::FindClass(className);
    if (!actorClass) {
        UE_LOGW("spawn_npc: class '%ls' not found in GUObjectArray -- ignoring", className);
        return;
    }
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        UE_LOGW("spawn_npc: no local player resolved yet -- ignoring (world loaded?)");
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("spawn_npc: no world context -- ignoring");
        return;
    }
    // Spawn point: ~250 cm ahead of the player's view yaw at the player's Z; the
    // NPC faces back toward the player (yaw+180) so it is framed in the host's view.
    const ue_wrap::FVector loc = E::GetActorLocation(local);
    float yaw = 0.f;
    if (void* ctrl = E::GetController(local)) yaw = E::GetControlRotation(ctrl).Yaw;
    constexpr float kDist = 250.f;
    constexpr float kDeg2Rad = 0.0174532925f;
    const float fwdX = std::cos(yaw * kDeg2Rad);
    const float fwdY = std::sin(yaw * kDeg2Rad);
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(0.f, yaw + 180.f, 0.f, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc.X + fwdX * kDist;
    xform.TY = loc.Y + fwdY * kDist;
    xform.TZ = loc.Z;

    constexpr uint8_t kAlwaysSpawn = 1;  // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    void* spawned = nullptr;
    {
        ParamFrame begin(refs.beginDeferredFn);
        if (!begin.valid()) {
            UE_LOGE("spawn_npc: ParamFrame(BeginDeferred) invalid");
            return;
        }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        // Dispatches through ProcessEvent -> npc_sync's NpcSuppress_Interceptor.
        // HOST: allocs the Npc Element (AllocAndInstall) + broadcasts EntitySpawn +
        // returns false (lets the spawn proceed). (Dev spawns are host-side.)
        if (!Call(refs.gsCdo, begin)) {
            UE_LOGE("spawn_npc: BeginDeferred call failed for '%ls'", className);
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("spawn_npc: BeginDeferred returned null for '%ls' (suppressed / not host?)",
                className);
        return;
    }
    {
        ParamFrame finish(refs.finishSpawnFn);
        if (!finish.valid()) {
            UE_LOGE("spawn_npc: ParamFrame(FinishSpawning) invalid -- actor %p half-spawned",
                    spawned);
            return;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(refs.gsCdo, finish)) {
            UE_LOGE("spawn_npc: FinishSpawningActor call failed for %p '%ls'", spawned, className);
            return;
        }
    }
    UE_LOGI("spawn_npc: spawned '%ls' actor=%p at (%.0f, %.0f, %.0f) (in front of local player %p)",
            className, spawned, xform.TX, xform.TY, xform.TZ, local);
}

void PostSpawnKerfur() {
    // The spawn touches engine reflection (BeginDeferred/FinishSpawning via
    // ProcessEvent) -- must run on the game thread.
    GT::Post([] { SpawnNpcInFront(P::name::NpcClass_KerfurOmega); });
}

// F7 hotkey + trigger-file watcher. F7 is foreground-gated (a same-box host+
// client both seeing the global key state would otherwise both fire from one
// press; the foreground gate attributes the press to the focused window). The
// trigger file is the deterministic autonomous path: mp.py creates it once all
// peers are connected; we spawn + delete it (re-create to spawn again).
DWORD WINAPI WatcherThread(LPVOID) {
    bool prevKey = false;
    wchar_t triggerPath[512] = {};
    ::GetEnvironmentVariableW(L"VOTVCOOP_SPAWN_TRIGGER", triggerPath, 512);
    const bool watchFile = (triggerPath[0] != L'\0');
    int fileTicks = 0;
    while (!coop::shutdown::IsShuttingDown()) {
        const bool key = ::coop::ini_config::IsOurWindowForeground() && KeyDown(VK_F7);
        if (key && !prevKey) {
            UE_LOGI("spawn_npc: F7 -> spawning kerfurOmega_C");
            PostSpawnKerfur();
        }
        prevKey = key;
        if (watchFile && ++fileTicks >= 32) {  // ~250 ms (32 * 8 ms)
            fileTicks = 0;
            if (::GetFileAttributesW(triggerPath) != INVALID_FILE_ATTRIBUTES) {
                UE_LOGI("spawn_npc: trigger file seen -> spawning kerfurOmega_C (autonomous)");
                PostSpawnKerfur();
                ::DeleteFileW(triggerPath);  // one-shot per file creation
            }
        }
        ::Sleep(8);
    }
    return 0;
}

}  // namespace

void Init() {
    if (!::coop::ini_config::MasterEnabled()) {
        UE_LOGI("spawn_npc: disabled by master switch ([dev] enabled=0)");
        return;
    }
    wchar_t probe[8] = {};
    const bool wantWatch =
        (::GetEnvironmentVariableW(L"VOTVCOOP_SPAWN_TRIGGER", probe, 8) > 0);
    const bool devkeys = ::coop::ini_config::IsIniKeyTrue("devkeys");
    if (!devkeys && !wantWatch) {
        UE_LOGI("spawn_npc: disabled (set [dev] devkeys=1 for F7, or "
                "VOTVCOOP_SPAWN_TRIGGER=<file> for the autonomous file trigger)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &WatcherThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached; loops until shutdown
    }
    UE_LOGI("spawn_npc: ENABLED (%s%s) -- spawns kerfurOmega_C in front of local player",
            devkeys ? "F7" : "", wantWatch ? " + file-trigger" : "");
}

}  // namespace coop::dev::spawn_npc
