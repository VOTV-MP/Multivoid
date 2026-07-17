// harness/autotest_ragdoll_spawn_probe.cpp -- SP-solo xray-ragdoll feasibility probe.
//
// The xray-ragdoll direction ([[project-ragdoll-sync]]): VOTV's real ragdollMode
// is LEAK-FREE but GLOBALLY scoped -- calling it kills the host's own player
// (death event regardless of params). So we cannot reuse ragdollMode on a puppet.
// The plan is to spawn VOTV's own `playerRagdoll_C` body MANUALLY (UWorld deferred
// spawn, NOT ragdollMode) on the puppet and xray it. This probe answers, by
// OBSERVING live objects in plain single-player (NO connection, role-agnostic) --
// per the 2026-06-01 rule "forget ue4ss, we use our c++ instead" -- the two
// decisive questions BEFORE any production code is written:
//
//   Q1  (manual spawn viable?): BeginDeferredSpawn<playerRagdoll_C> + set Player
//        @0x248 (Expose-On-Spawn) + FinishDeferredSpawn -- WITHOUT ragdollMode --
//        and check whether the actor's SkeletalMesh @0x230 component ends up
//        VISIBLE + PHYSICALLY SIMULATING (IsAnyRigidBodyAwake) and the body FALLS
//        (lowest-bone Z drops over a few ticks). I.e. does AplayerRagdoll_C
//        SELF-CONFIGURE in its own ReceiveBeginPlay from the Player ref?
//   Q1b (death-free?): does the manual spawn trigger a DEATH/faint on the Player
//        we set? (read mainPlayer.dead + isRagdoll before/after; screenshot the
//        un-faded screen). The host-death bleed lived in ragdollMode (global); the
//        hypothesis is the actor's own BeginPlay does NOT carry it. This confirms
//        or refutes that -- the whole feature hinges on it.
//   Q2  (ground truth): trigger the REAL ragdollMode(true,false,false), grab
//        mainPlayer.ragdollActor @0xC40, and dump the SAME fields -> the target
//        config to match. forceGetUp() to recover.
//
// Single instance: launch via `mp.py ragdollspawn` (solo host, no client).
// Screenshots land in research/ragdoll_shots/ on the "MANUAL-SHOT READY" /
// "REAL-SHOT READY" log markers. Gated by env VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE=1
// (registered in autotest_dispatch.cpp). Throwaway diagnostic: raw offset reads
// on AplayerRagdoll_C (Player @0x248, SkeletalMesh comp @0x230) and mainPlayer
// (ragdollActor @0xC40) are cited to the CXX SDK dump, mirroring the sibling
// autotest_vitals.cpp raw-ParamFrame diagnostics -- this is not a shipping path.

#include "harness/autotest.h"
#include "harness/screenshot.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

// --- CXX SDK offsets (Game_0.9.0n CXXHeaderDump; cited, raw diagnostic) ---
constexpr size_t kAragdoll_SkeletalMesh = 0x0230;  // Aragdoll_C::SkeletalMesh (USkeletalMeshComponent*)
constexpr size_t kPlayerRagdoll_Player  = 0x0248;  // AplayerRagdoll_C::Player (AmainPlayer_C*)
constexpr size_t kMainPlayer_ragdollActor = 0x0C40;  // AmainPlayer_C::ragdollActor (AplayerRagdoll_C*)
constexpr size_t kUSkinnedMesh_SkeletalMesh = 0x0480;  // USkinnedMeshComponent::SkeletalMesh (the skin asset)

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

// Bounded spin-wait on a game-thread task's completion flag (mirrors
// autotest_vitals.cpp). false => the posted task faulted (SEH firewall ate the
// AV, flag never set) so the caller bails instead of hanging the probe.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// Call a no-arg bool UFunction (e.g. IsVisible / IsAnyRigidBodyAwake) on `comp`.
// Game thread only. Returns false if unresolved/uncallable.
bool CallBoolFn(void* comp, const wchar_t* cls, const wchar_t* fn) {
    if (!comp || !R::IsLive(comp)) return false;
    void* f = R::FindFunction(R::FindClass(cls), fn);
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    if (!pf.valid()) return false;
    return ue_wrap::Call(comp, pf) && pf.Get<bool>(L"ReturnValue");
}

// Start the PhysX gravity simulation on a skeletal-mesh component (collision so
// the bodies have a floor to rest on, then SetAllBodiesSimulatePhysics(true) +
// SetSimulatePhysics(true)). This is the step ragdollMode does AFTER spawn that
// AplayerRagdoll_C::BeginPlay does NOT -- the bare spawn builds the bodies but
// leaves them frozen. Game thread only. Throwaway diagnostic (raw UFunction calls).
void StartBodySim(void* comp) {
    if (!comp || !R::IsLive(comp)) return;
    if (void* f = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"SetCollisionEnabled")) {
        ue_wrap::ParamFrame pf(f); pf.Set<uint8_t>(L"NewType", uint8_t{3});  // QueryAndPhysics
        ue_wrap::Call(comp, pf);
    }
    if (void* f = R::FindFunction(R::FindClass(L"SkeletalMeshComponent"), L"SetAllBodiesSimulatePhysics")) {
        ue_wrap::ParamFrame pf(f); pf.Set<bool>(L"bNewSimulate", true); ue_wrap::Call(comp, pf);
    }
    if (void* f = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"SetSimulatePhysics")) {
        ue_wrap::ParamFrame pf(f); pf.Set<bool>(L"bSimulate", true); ue_wrap::Call(comp, pf);
    }
}

// The SkeletalMesh component @0x230 on an Aragdoll_C/AplayerRagdoll_C actor.
void* RagdollMeshComp(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + kAragdoll_SkeletalMesh);
    return (c && R::IsLive(c)) ? c : nullptr;
}

// Dump the full live configuration of a ragdoll actor (manual or real) so the
// two can be compared field-for-field. Game thread only.
void DumpRagdollActor(void* actor, const char* tag) {
    if (!actor || !R::IsLive(actor)) {
        UE_LOGW("ragdollspawn[%s]: actor null/dead -- nothing to dump", tag);
        return;
    }
    const std::wstring cls = R::ClassNameOf(actor);
    const ue_wrap::FVector aLoc = E::GetActorLocation(actor);
    void* comp = RagdollMeshComp(actor);
    if (!comp) {
        UE_LOGW("ragdollspawn[%s]: actor '%ls' has NO live SkeletalMesh @0x230 (actorZ=%.0f)",
                tag, cls.c_str(), aLoc.Z);
        return;
    }
    void* meshAsset = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(comp) + kUSkinnedMesh_SkeletalMesh);
    const std::wstring meshName = (meshAsset && R::IsLive(meshAsset))
                                      ? R::ToString(R::NameOf(meshAsset)) : L"<null>";
    const bool vis    = CallBoolFn(comp, L"SceneComponent", L"IsVisible");
    const bool awake  = CallBoolFn(comp, L"PrimitiveComponent", L"IsAnyRigidBodyAwake");
    const ue_wrap::FVector cLoc = E::GetComponentLocation(comp);
    float lowZ = 0.f;
    const bool okBone = E::GetLowestBoneWorldZ(comp, lowZ);

    // Material slot-0 (what the body renders with today -> the xray swap target).
    std::wstring mat0 = L"<?>";
    int32_t numMat = 0;
    if (void* gn = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"GetNumMaterials")) {
        ue_wrap::ParamFrame f(gn);
        if (ue_wrap::Call(comp, f)) numMat = f.Get<int32_t>(L"ReturnValue");
    }
    if (void* gm = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"GetMaterial")) {
        ue_wrap::ParamFrame f(gm); f.Set<int32_t>(L"ElementIndex", 0);
        if (ue_wrap::Call(comp, f)) { void* m = f.Get<void*>(L"ReturnValue");
            if (m && R::IsLive(m)) mat0 = R::ToString(R::NameOf(m)); }
    }

    UE_LOGI("ragdollspawn[%s]: actor='%ls' actorZ=%.0f | comp meshAsset='%ls' IsVisible=%d "
            "IsAnyRigidBodyAwake=%d compZ=%.0f lowestBoneZ=%.0f(ok=%d) numMaterials=%d mat0='%ls'",
            tag, cls.c_str(), aLoc.Z, meshName.c_str(), vis ? 1 : 0, awake ? 1 : 0,
            cLoc.Z, lowZ, okBone ? 1 : 0, numMat, mat0.c_str());
}

// Aim the local player's camera at a target actor's mesh so the autonomous
// screenshot frames the spawned body. Game thread only.
void AimLocalAt(void* local, void* target) {
    if (!local || !R::IsLive(local) || !target || !R::IsLive(target)) return;
    void* ctrl = E::GetController(local);
    if (!ctrl || !R::IsLive(ctrl)) return;
    const ue_wrap::FVector h = E::GetActorLocation(local);
    void* comp = RagdollMeshComp(target);
    const ue_wrap::FVector p = comp ? E::GetComponentLocation(comp) : E::GetActorLocation(target);
    const float dx = p.X - h.X, dy = p.Y - h.Y, dz = p.Z - (h.Z + 60.f);
    const float horiz = std::sqrt(dx * dx + dy * dy);
    const float yaw = std::atan2(dy, dx) * 57.29578f;
    const float pitch = std::atan2(dz, horiz) * 57.29578f;
    E::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, yaw, 0.f});
}

// Read mainPlayer.dead + isRagdoll (Q1b death/faint detection). Returns false if
// unreadable. Game thread only.
bool ReadDeadRagdoll(void* mp, bool& dead, bool& isRagdoll) {
    return E::ReadMainPlayerRagdollState(mp, isRagdoll, dead);
}

// Sample whether a body is FALLING: log component Z + lowest-bone Z (a drop over
// successive samples == real physics; static == not simulating). Game thread.
void SampleFall(void* actor, const char* tag) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([actor, tag, done] {
        if (actor && R::IsLive(actor)) {
            void* comp = RagdollMeshComp(actor);
            const float cZ = comp ? E::GetComponentLocation(comp).Z : 0.f;
            float lowZ = 0.f;
            const bool ok = comp ? E::GetLowestBoneWorldZ(comp, lowZ) : false;
            UE_LOGI("ragdollspawn[%s]: fall-sample compZ=%.1f lowestBoneZ=%.1f(ok=%d)",
                    tag, cZ, lowZ, ok ? 1 : 0);
        }
        done->store(1);
    });
    WaitDone(done, 8000);
}

// ====================== EXPERIMENT B: manual spawn ======================
// Returns the spawned actor (kept alive for the screenshot), or nullptr.
void* SpawnManualRagdoll(void* local) {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out  = std::make_shared<void*>(nullptr);
    GT::Post([local, done, out] {
        void* cls = R::FindClass(L"playerRagdoll_C");
        if (!cls) { UE_LOGW("ragdollspawn[manual]: playerRagdoll_C class did not resolve"); done->store(1); return; }
        const ue_wrap::FVector pl = E::GetActorLocation(local);
        const ue_wrap::FVector fwd = E::GetActorForwardVector(local);
        const ue_wrap::FRotator rot = E::GetActorRotation(local);
        // ~120u in front of the player at the same Z (framable; not inside the body).
        const ue_wrap::FVector loc{ pl.X + fwd.X * 120.f, pl.Y + fwd.Y * 120.f, pl.Z };
        void* actor = E::BeginDeferredSpawn(cls, loc, rot);
        if (!actor) { UE_LOGW("ragdollspawn[manual]: BeginDeferredSpawn returned null"); done->store(1); return; }
        // Expose-On-Spawn: set Player @0x248 BEFORE FinishDeferredSpawn runs
        // BeginPlay, so the actor's own ReceiveBeginPlay sees its owning player.
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + kPlayerRagdoll_Player) = local;
        E::FinishDeferredSpawn(actor, loc, rot);
        // Root it for the probe's lifetime so an incidental GC pass cannot reap the
        // body mid-experiment and turn a working spawn into a false "null/dead" dump
        // (audit 2026-06-01). Intentionally left rooted -- this is a throwaway probe
        // whose session is killed right after; DestroyActor below tears the body down
        // regardless of root, and a self-destructing BeginPlay still shows as dead.
        R::AddToRoot(actor);
        UE_LOGI("ragdollspawn[manual]: spawned playerRagdoll_C @%p at (%.0f,%.0f,%.0f), Player set -- NO ragdollMode called",
                actor, loc.X, loc.Y, loc.Z);
        *out = actor;
        done->store(1);
    });
    WaitDone(done, 8000);
    return *out;
}

void RunManualExperiment(void* local) {
    UE_LOGI("ragdollspawn: === EXPERIMENT B (manual spawn, NO ragdollMode) ===");

    // Baseline dead/isRagdoll (Q1b: must be unchanged after the manual spawn).
    bool deadBefore = false, ragBefore = false;
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto d = std::make_shared<int>(0), r = std::make_shared<int>(0);
        GT::Post([local, done, d, r] {
            bool dead = false, isRag = false;
            if (ReadDeadRagdoll(local, dead, isRag)) { *d = dead ? 1 : 0; *r = isRag ? 1 : 0; }
            done->store(1);
        });
        WaitDone(done, 8000);
        deadBefore = *d != 0; ragBefore = *r != 0;
    }
    UE_LOGI("ragdollspawn[manual]: player BEFORE spawn -- dead=%d isRagdoll=%d", deadBefore ? 1 : 0, ragBefore ? 1 : 0);

    void* actor = SpawnManualRagdoll(local);
    if (!actor) {
        UE_LOGW("ragdollspawn[manual]: VERDICT FAIL -- could not spawn playerRagdoll_C manually");
        return;
    }

    ::Sleep(1500);  // let ReceiveBeginPlay + the first ticks run (BARE -- no sim yet)

    // Dump the BARE configured body + the Q1b death check (this is what BeginPlay
    // alone produces -- expected visible but frozen).
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto d = std::make_shared<int>(0), r = std::make_shared<int>(0);
        GT::Post([actor, local, done, d, r] {
            DumpRagdollActor(actor, "manual-bare");
            bool dead = false, isRag = false;
            if (ReadDeadRagdoll(local, dead, isRag)) { *d = dead ? 1 : 0; *r = isRag ? 1 : 0; }
            done->store(1);
        });
        WaitDone(done, 8000);
        UE_LOGI("ragdollspawn[manual]: player AFTER bare spawn -- dead=%d isRagdoll=%d (Q1b: %s)",
                *d, *r, (*d == (deadBefore ? 1 : 0) && *r == (ragBefore ? 1 : 0))
                            ? "UNCHANGED -> manual spawn is DEATH-FREE"
                            : "CHANGED -> the actor's BeginPlay bleeds player state");
    }
    SampleFall(actor, "manual-bare");  // expected static (BeginPlay doesn't sim)

    // START THE SIM ourselves -- the step ragdollMode does after spawn. If the body
    // now FALLS (lowestBoneZ drops toward the floor), the recipe is: deferred spawn
    // + set Player + Finish + StartBodySim. That is the whole feature.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([actor, local, done] {
            void* comp = RagdollMeshComp(actor);
            StartBodySim(comp);
            UE_LOGI("ragdollspawn[manual]: started physics sim on the body mesh (SetAllBodiesSimulatePhysics+SetSimulatePhysics)");
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    ::Sleep(900);  // let it begin falling before we frame + dump
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([actor, local, done] {
            DumpRagdollActor(actor, "manual-sim");
            AimLocalAt(local, actor);
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    UE_LOGI("ragdollspawn[manual]: MANUAL-SHOT READY");  // mp.py captures here
    ::Sleep(1200);

    // Does it FALL now that the sim is started? Sample geometry across ~3 s.
    SampleFall(actor, "manual-sim");
    ::Sleep(1500); SampleFall(actor, "manual-sim");
    ::Sleep(1500); SampleFall(actor, "manual-sim");

    // Clean up so it doesn't confuse the real-ragdoll comparison frame.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([actor, done] {
            if (actor && R::IsLive(actor)) { E::DestroyActor(actor);
                UE_LOGI("ragdollspawn[manual]: destroyed the manual ragdoll actor"); }
            done->store(1);
        });
        WaitDone(done, 8000);
    }
}

// ====================== EXPERIMENT A: real ragdollMode ======================
void RunRealExperiment(void* local) {
    UE_LOGI("ragdollspawn: === EXPERIMENT A (real ragdollMode, ground truth) ===");

    // Fire the real ragdollMode on the LOCAL possessed player (in plain SP this
    // is the normal faint mechanic; forceGetUp recovers it below).
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, done] {
            const bool ok = E::SetMainPlayerRagdollMode(local, /*ragdoll=*/true, /*passOut=*/false, /*death=*/false);
            UE_LOGI("ragdollspawn[real]: ragdollMode(true,false,false) dispatched=%d", ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }

    ::Sleep(1500);  // let the real playerRagdoll_C spawn + configure

    // Grab mainPlayer.ragdollActor @0xC40 -> dump it + frame it.
    auto real = std::make_shared<void*>(nullptr);
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, real, done] {
            void* ra = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(local) + kMainPlayer_ragdollActor);
            if (ra && R::IsLive(ra)) {
                *real = ra;
                DumpRagdollActor(ra, "real");
                AimLocalAt(local, ra);
            } else {
                UE_LOGW("ragdollspawn[real]: mainPlayer.ragdollActor @0xC40 null/dead after ragdollMode");
            }
            bool dead = false, isRag = false;
            if (ReadDeadRagdoll(local, dead, isRag))
                UE_LOGI("ragdollspawn[real]: player dead=%d isRagdoll=%d after ragdollMode", dead ? 1 : 0, isRag ? 1 : 0);
            // Ragdoll bone visualizer chain verify (2026-07-03): exercise the EXACT accessor +
            // bone-graph path the overlay uses, in-process, while the real ragdoll is live --
            // the autonomous screenshot keeps missing the ~4 s auto-getup window, so this log
            // line is the deterministic proof the overlay would have drawn N bones.
            {
                void* mesh = E::GetLocalRagdollBodyMesh(local);
                std::vector<E::BonePoint> pts;
                const int nb = E::CollectSkeletonBonePoints(mesh, pts);
                int parented = 0;
                for (const auto& b : pts) if (b.parent >= 0) ++parented;
                UE_LOGI("ragdollspawn[real]: [BONE-OVERLAY-CHAIN] localRagdollMesh=%p bones=%d "
                        "parented-lines=%d (the ragdoll_bone_overlay draw set; >0 bones = chain VERIFIED)",
                        mesh, nb, parented);
            }
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    UE_LOGI("ragdollspawn[real]: REAL-SHOT READY");  // mp.py captures here
    ::Sleep(1200);

    if (*real) { SampleFall(*real, "real"); ::Sleep(1500); SampleFall(*real, "real"); }

    // Recover so the session stays clean.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, done] {
            const bool ok = E::ForceMainPlayerGetUp(local);
            UE_LOGI("ragdollspawn[real]: forceGetUp dispatched=%d", ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }
}

}  // namespace

void RunRagdollSpawnProbe() {
    UE_LOGI("ragdollspawn: probe armed -- SP-solo xray-ragdoll feasibility (manual spawn vs real ragdollMode)");

    // Wait for a live local possessed mainPlayer_C (post-load). Poll up to 90 s
    // (the `play` scenario loads s_may2026 then enters gameplay).
    auto local = std::make_shared<void*>(nullptr);
    for (int attempt = 0; attempt < 90 && !*local; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, done] {
            void* mp = coop::players::Registry::Get().Local();
            if (mp && R::IsLive(mp)) *local = mp;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (!*local) ::Sleep(1000);
    }
    if (!*local) {
        UE_LOGW("ragdollspawn: VERDICT INCONCLUSIVE -- no local player in 90 s; cannot probe");
        return;
    }
    UE_LOGI("ragdollspawn: local player resolved -- letting the world settle 5 s");
    ::Sleep(5000);

    // Diagnostic: is playerRagdoll_C already resident, or only loaded lazily when
    // ragdollMode first fires? (Tells us whether the real-first ordering below was
    // load-bearing or just belt-and-suspenders.)
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([done] {
            void* cls = R::FindClass(L"playerRagdoll_C");
            UE_LOGI("ragdollspawn: playerRagdoll_C resident at settle = %s",
                    cls ? "YES (already loaded)" : "NO (lazy -- the real experiment loads it first)");
            done->store(1);
        });
        WaitDone(done, 8000);
    }

    // REAL experiment FIRST: ragdollMode loads the playerRagdoll_C BP class as a
    // side effect, so the MANUAL experiment's FindClass is guaranteed to resolve
    // (BP classes load lazily; running manual first risks a silent class-not-found
    // false negative -- audit 2026-06-01). It also yields the ground-truth dump.
    RunRealExperiment(*local);
    ::Sleep(2500);  // let forceGetUp recovery settle + ragdollActor@0xC40 clear
    RunManualExperiment(*local);

    UE_LOGI("ragdollspawn: DONE -- compare [manual] vs [real] dumps above. Manual viable iff its "
            "body IsVisible=1 + IsAnyRigidBodyAwake=1 + lowestBoneZ falls AND Q1b says DEATH-FREE.");
}

DWORD WINAPI RagdollSpawnProbeThread(LPVOID) {
    RunRagdollSpawnProbe();
    return 0;
}

}  // namespace harness::autotest
