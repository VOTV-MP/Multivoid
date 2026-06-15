// coop/dev/pinecone_probe.cpp -- see header.

#include "coop/dev/pinecone_probe.h"

#include "coop/ini_config.h"
#include "coop/npc_sync.h"            // GetDevSpawnRefs (generic GameplayStatics spawn fns)
#include "coop/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::dev::pinecone_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool  g_enabled = false;
bool  g_checked = false;   // ini read once
bool  g_fired   = false;   // one-shot
std::chrono::steady_clock::time_point g_armAt{};
bool  g_armed = false;

// Force-spawn ONE prop_food_pinecone_C ~250 cm in front of the local (host)
// player, the same way pineconeSpawner does: BeginDeferredActorSpawnFromClass +
// FinishSpawningActor (whose UCS runs the prop's Init -> our host Init-POST
// observer fires -> SendPropSpawn, per the RE). No SetName: this class carries
// its own mesh (unlike base prop_C), so the broadcast/mirror trigger is what we
// test, and propName carry-through is already proven by the cubicle walls.
// Force-spawn `className` ~250 cm in front of the local (host) player via the
// SAME BeginDeferredActorSpawnFromClass + FinishSpawningActor two-step every
// game spawner (and the sandbox Q-menu) uses -- whose UCS runs the prop's init().
// This exercises BOTH host_spawn_watcher seams in one path. Returns the spawned
// actor (or null). `watchHint` names the host+client log lines to look for.
void* ForceSpawnAt(const wchar_t* className, float zOffset, const char* watchHint) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    coop::npc_sync::DevSpawnRefs refs;
    if (!coop::npc_sync::GetDevSpawnRefs(refs)) {
        UE_LOGW("pinecone_probe: spawn refs not ready -- retrying next arm");
        return nullptr;
    }
    void* cls = R::FindClass(className);
    if (!cls) { UE_LOGW("pinecone_probe: %ls not in GUObjectArray yet", className); return nullptr; }
    void* local = coop::players::Registry::Get().Local();
    if (!local) { UE_LOGW("pinecone_probe: no local player"); return nullptr; }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) { UE_LOGW("pinecone_probe: no world context"); return nullptr; }

    const ue_wrap::FVector loc = E::GetActorLocation(local);
    float yaw = 0.f;
    if (void* ctrl = E::GetController(local)) yaw = E::GetControlRotation(ctrl).Yaw;
    constexpr float kDist = 250.f, kDeg2Rad = 0.0174532925f;
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(0.f, yaw, 0.f, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc.X + std::cos(yaw * kDeg2Rad) * kDist;
    xform.TY = loc.Y + std::sin(yaw * kDeg2Rad) * kDist;
    xform.TZ = loc.Z + zOffset;

    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(refs.beginDeferredFn);
        if (!begin.valid()) { UE_LOGE("pinecone_probe: ParamFrame(BeginDeferred) invalid"); return nullptr; }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", cls);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(refs.gsCdo, begin)) { UE_LOGE("pinecone_probe: BeginDeferred failed for %ls", className); return nullptr; }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) { UE_LOGE("pinecone_probe: BeginDeferred returned null for %ls", className); return nullptr; }
    {
        ParamFrame finish(refs.finishSpawnFn);
        if (!finish.valid()) { UE_LOGE("pinecone_probe: ParamFrame(Finish) invalid"); return nullptr; }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(refs.gsCdo, finish)) { UE_LOGE("pinecone_probe: FinishSpawningActor failed for %ls", className); return nullptr; }
    }
    UE_LOGI("pinecone_probe: FORCE-SPAWNED %ls actor=%p at (%.0f,%.0f,%.0f) -- %s",
            className, spawned, xform.TX, xform.TY, xform.TZ, watchHint);
    return spawned;
}

// Spawn BOTH host_spawn_watcher test cases in one shot:
//   (1) KEYLESS ambient -- prop_food_pinecone_C: caught at the BeginDeferred POST
//       seam (it carries its own mesh, drops + bounces = the scare) -> keyless
//       PropSpawn + death-watch. (No SetName needed.)
//   (2) KEYED sandbox  -- bare prop_C: its own init() mints a random Key during
//       FinishSpawningActor (the exact Q-menu / toolgun keyed path), but that
//       init is BP-internal so prop_lifecycle's Init-POST observer NEVER sees it
//       -> caught at the FinishSpawningActor POST seam -> prop_lifecycle's keyed
//       broadcast -> client mirrors KEYED. (Spawns the CDO 'cube' mesh; the KEY
//       + keyed-mirror mechanism is what's under test, not the mesh.)
void ForceSpawnProbeProps() {
    ForceSpawnAt(L"prop_food_pinecone_C", 150.f,
                 "KEYLESS: watch HOST 'host_spawn_watcher: MIRROR ambient spawn cls=prop_food_pinecone_C' "
                 "+ CLIENT 'OnSpawn: cls=prop_food_pinecone_C' key=''");
    ForceSpawnAt(L"prop_C", 220.f,
                 "KEYED: watch HOST 'grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls=prop_C key=<non-None>' "
                 "+ CLIENT 'OnSpawn: cls=prop_C' with a non-empty key (the Q-menu FinishSpawningActor seam)");
}

}  // namespace

void Install() {
    if (g_checked) return;
    g_checked = true;
    g_enabled = ::coop::ini_config::MasterEnabled() &&
                ::coop::ini_config::IsIniKeyTrue("pinecone_probe");
    if (g_enabled) UE_LOGI("pinecone_probe: ARMED (ini pinecone_probe=1) -- host force-spawns a KEYLESS pinecone + a KEYED prop_C ~45s after a client connects (tests both host_spawn_watcher seams)");
}

void Tick(bool isConnected, bool isHost) {
    if (!g_enabled || g_fired || !isHost) return;
    if (!isConnected) { g_armed = false; return; }  // wait for a client
    const auto now = std::chrono::steady_clock::now();
    // Fire WELL AFTER the connect-snapshot bracket completes (~30 s for a heavy
    // save) so we test the clean LIVE Init-POST broadcast path in isolation --
    // a spawn during the bracket gets swept into the snapshot instead (the
    // first probe run's confound: pinecone arrived 30 s late + at rest via the
    // drain, not a live broadcast).
    if (!g_armed) { g_armed = true; g_armAt = now + std::chrono::seconds(45); return; }
    if (now < g_armAt) return;
    g_fired = true;
    ForceSpawnProbeProps();
}

}  // namespace coop::dev::pinecone_probe
