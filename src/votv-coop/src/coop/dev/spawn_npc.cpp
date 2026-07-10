// coop/dev/spawn_npc.cpp -- see coop/dev/spawn_npc.h.

#include "coop/dev/spawn_npc.h"

#include "coop/dev/dev_gate.h"
#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"   // RemotePlayer::GetActor (the on-client wisp test spawn)
#include "coop/session/shutdown.h"
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

// Spawn an allowlisted NPC at `xform` via BeginDeferredActorSpawnFromClass +
// FinishSpawningActor. The spawn goes through the very UFunction npc_sync's interceptor
// hooks, so on the HOST the interceptor runs the full sync path (AllocAndInstall an Npc
// Element + broadcast EntitySpawn + let the spawn proceed) and connected clients
// materialize a mirror. This is the ONLY programmatic NPC-spawn trigger (VOTV NPCs spawn
// only from purchase/scripted events). Game thread only. (Extracted from npc_sync.cpp
// 2026-05-30; the spawn refs come from npc_sync::GetDevSpawnRefs.)
void SpawnNpcAt(const wchar_t* className, const ue_wrap::FTransform& xform) {
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
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("spawn_npc: no world context -- ignoring");
        return;
    }
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
    UE_LOGI("spawn_npc: spawned '%ls' actor=%p at (%.0f, %.0f, %.0f)",
            className, spawned, xform.TX, xform.TY, xform.TZ);
}

// Spawn ~2.5 m in front of the local player at their view yaw (NO turn-to-face -- the
// client mirrors the transform byte-for-byte).
void SpawnNpcInFront(const wchar_t* className) {
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        UE_LOGW("spawn_npc: no local player resolved yet -- ignoring (world loaded?)");
        return;
    }
    const ue_wrap::FVector loc = E::GetActorLocation(local);
    float yaw = 0.f;
    if (void* ctrl = E::GetController(local)) yaw = E::GetControlRotation(ctrl).Yaw;
    constexpr float kDist = 250.f;
    constexpr float kDeg2Rad = 0.0174532925f;
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(0.f, yaw, 0.f, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc.X + std::cos(yaw * kDeg2Rad) * kDist;
    xform.TY = loc.Y + std::sin(yaw * kDeg2Rad) * kDist;
    xform.TZ = loc.Z;
    SpawnNpcAt(className, xform);
}

// v72 Killer Wisp TEST hook: spawn `className` ON the first connected client puppet so it
// immediately acquires the PUPPET as Target -- the only easy way to exercise the cross-peer
// kill (the wisp normally targets whoever is nearest, = the host). Host-side; no-op + warns
// if no client puppet is present. Game thread.
void SpawnNpcOnFirstClient(const wchar_t* className) {
    auto& reg = coop::players::Registry::Get();
    void* puppetActor = nullptr;
    uint8_t foundSlot = 0;
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (coop::RemotePlayer* pp = reg.Puppet(slot)) {
            void* a = pp->GetActor();
            if (a && R::IsLive(a)) { puppetActor = a; foundSlot = slot; break; }
        }
    }
    if (!puppetActor) {
        UE_LOGW("spawn_npc: no connected client puppet -- cannot spawn '%ls' on a client", className);
        return;
    }
    const ue_wrap::FVector loc = E::GetActorLocation(puppetActor);
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(0.f, 0.f, 0.f, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc.X;
    xform.TY = loc.Y;
    xform.TZ = loc.Z + 50.f;  // slightly up so it doesn't clip the floor
    UE_LOGI("spawn_npc: spawning '%ls' ON client slot %u puppet at (%.0f, %.0f, %.0f)",
            className, foundSlot, loc.X, loc.Y, loc.Z);
    SpawnNpcAt(className, xform);
}

void PostSpawnKerfur() {
    // Strict client lockout: a client-side spawn is LOCAL-ONLY (host-auth NPC
    // pipeline never registers it) -- a ghost entity + a cheat (coop::dev_gate).
    // Gating here covers both entries: the F1 menu button and the autonomous
    // file trigger.
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("spawn_npc: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    // The spawn touches engine reflection (BeginDeferred/FinishSpawning via
    // ProcessEvent) -- must run on the game thread.
    GT::Post([] { SpawnNpcInFront(P::name::NpcClass_KerfurOmega); });
}

// Generic dev test-spawn of an allowlisted class in front of the host player.
// THE reliable way to test a new npc_sync allowlist entry: F1 EVENTS are a poor
// test (wisps only arms an overlap box; ventCrawler's eventer spawn uses
// EX_CallMath -> bypasses our ProcessEvent interceptor + lands ~10 m away in a
// vent). This Call->reflection::CallFunction->ProcessEvent path re-enters the
// SAME detoured pointer, so NpcSuppress_Interceptor fires and the host broadcasts
// EntitySpawn -> the client mirrors it. Client-locked (dev_gate); game thread.
// className is a static string-literal constant (sdk_profile) -> safe to capture.
void PostSpawnClass(const wchar_t* className) {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("spawn_npc: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    GT::Post([className] { SpawnNpcInFront(className); });
}

// Trigger-FILE watcher (autonomous path only). mp.py creates the file once all
// peers are connected; we spawn + delete it (re-create to spawn again). The
// hands-on path is the dev menu's SpawnKerfurOmega button -- no key polling here.
DWORD WINAPI FileTriggerThread(LPVOID) {
    wchar_t triggerPath[512] = {};
    ::GetEnvironmentVariableW(L"VOTVCOOP_SPAWN_TRIGGER", triggerPath, 512);
    if (triggerPath[0] == L'\0') return 0;  // nothing to watch
    int fileTicks = 0;
    while (!coop::shutdown::IsShuttingDown()) {
        if (++fileTicks >= 32) {  // ~250 ms (32 * 8 ms)
            fileTicks = 0;
            if (::GetFileAttributesW(triggerPath) != INVALID_FILE_ATTRIBUTES) {
                UE_LOGI("spawn_npc: trigger file seen -> spawning kerfurOmega_C (autonomous)");
                PostSpawnKerfur();
                ::DeleteFileW(triggerPath);  // one-shot per file creation
            }
        }
        ::Sleep(16);  // 60 Hz (user-set 2026-06-04, was 125)
    }
    return 0;
}

}  // namespace

void SpawnKerfurOmega() { PostSpawnKerfur(); }
void SpawnKillerWisp()  { PostSpawnClass(P::name::NpcClass_KillerWisp); }
void SpawnVentCrawler() { PostSpawnClass(P::name::NpcClass_VentCrawler); }
// v108 OWNER-ENTITY test: eyer_C is NOT in the npc allowlist by design -- the
// interceptor passes the spawn through and owner_entity_sync's own BeginDeferred
// POST observer catches it as a LOCALLY-OWNED entity + announces it to peers
// (the per-peer-owned + cross-peer-visible lane). Spawning it here is the F1
// end-to-end test of that lane.
void SpawnEyer()        { PostSpawnClass(L"eyer_C"); }

void SpawnKillerWispOnClient() {
    // v72 Killer Wisp cross-peer-kill test: spawn the wisp ON the first client puppet so it
    // grabs the PUPPET (routes the kill to that client) rather than the host. Host-side only.
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("spawn_npc: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    GT::Post([] { SpawnNpcOnFirstClient(P::name::NpcClass_KillerWisp); });
}

void Init() {
    wchar_t probe[8] = {};
    const bool wantWatch =
        (::GetEnvironmentVariableW(L"VOTVCOOP_SPAWN_TRIGGER", probe, 8) > 0);
    if (!wantWatch) {
        UE_LOGI("spawn_npc: file watcher off (no VOTVCOOP_SPAWN_TRIGGER) -- "
                "spawn via the F1 menu (Content > Entities)");
        return;
    }
    if (!::coop::config::MasterEnabled()) {
        UE_LOGI("spawn_npc: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &FileTriggerThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached; loops until shutdown
    }
    UE_LOGI("spawn_npc: file-trigger ENABLED (VOTVCOOP_SPAWN_TRIGGER) -- spawns kerfurOmega_C");
}

}  // namespace coop::dev::spawn_npc
