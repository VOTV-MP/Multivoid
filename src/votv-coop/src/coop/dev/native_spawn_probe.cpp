// coop/dev/native_spawn_probe.cpp -- see coop/dev/native_spawn_probe.h.

#include "coop/dev/native_spawn_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/npc_sync.h"
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // PropertyOffsetAndClass
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::native_spawn_probe {

UE_FACT_FN(kRowNb0Spawn, "dev.native_spawn_probe", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;

enum class Mode : unsigned char { Off, Record, Orb };
// Verify: the recorded orbs were destroyed and the spawners restored; the next Tick checks the destroy held.
enum class Orb : unsigned char { Wait, Drive, Verify, Done };

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
constexpr uint64_t kOrbBoundMs = 60000;  // the failure bound: a spawn needs a nav point near the camera
constexpr int32_t  kOrbLaps = 241;       // blackballSpawner_C::maxlaps; the next wrap passes it
constexpr float    kOrbAngle = 540.f;    // past 360, so the next tick's wrap test is true

Mode ModeOf() {
    static const Mode mode = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::native_spawn_probe);
        return v == "record" ? Mode::Record : v == "orb" ? Mode::Orb : Mode::Off;
    }();
    return mode;
}

// A spawner the drive wrote to, with the angle it held before the first write.
struct Spawner {
    ue_wrap::CachedObjRef ref;
    float                 angle;
};

coop::net::Session* g_session = nullptr;
bool     g_hookDone = false;
bool     g_hookFailed = false;  // latched for the process: a hook that failed kTries times does not heal
int      g_tries = 0;
uint64_t g_triedAt = 0;
void*    g_gsCdo = nullptr;  // an engine CDO: lives for the process
Key      g_keys[kMaxKeys] = {};
int      g_nKeys = 0;
bool     g_fullSaid = false;

Orb      g_orb = Orb::Wait;
std::vector<ue_wrap::CachedObjRef> g_orbs;  // every npc_orborb_C the record saw from a blackballSpawner_C
std::vector<Spawner> g_spawners;
uint64_t g_orbStartAt = 0;  // the first post-sweep Tick; 0 until then
int32_t  g_lapsOff = -1, g_angleOff = -1;
int      g_orbInstances = 0;

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

// The post hook: game thread, deep inside the engine spawn, so allocation-free until a new key prints or an
// orb is held.
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

    if (g_orb == Orb::Drive && R::NameEquals(probe.src, L"blackballSpawner_C") &&
        R::NameEquals(probe.out, L"npc_orborb_C"))
        g_orbs.emplace_back().Set(result);
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
        UE_LOGI("[NB-0] record live mode=%s", ModeOf() == Mode::Orb ? "orb" : "record");
    } else if (final) {
        g_hookFailed = true;
        UE_LOGI("[NB-0] BROKEN hook BeginDeferredActorSpawnFromClass");
    }
}

// ---- the orb drive ----

// Every blackballSpawner_C gets the drive's values; the angle each held before its first write is kept.
void DriveInstance(void*, void* obj, int32_t) {
    auto* base = static_cast<uint8_t*>(obj);
    bool  known = false;
    for (const Spawner& s : g_spawners) known = known || s.ref.Is(obj);
    if (!known) {
        g_spawners.push_back(Spawner{{}, *reinterpret_cast<float*>(base + g_angleOff)});
        g_spawners.back().ref.Set(obj);
    }
    *reinterpret_cast<int32_t*>(base + g_lapsOff) = kOrbLaps;
    *reinterpret_cast<float*>(base + g_angleOff) = kOrbAngle;
}

// The count of spawners driven.
int DriveSpawners(void* cls) { return static_cast<int>(OI::ForEachInstance(cls, &DriveInstance, nullptr)); }

// Every spawner the drive wrote to goes back: laps 0, the angle it held; one that is gone is skipped.
void RestoreSpawners() {
    for (const Spawner& s : g_spawners) {
        auto* const base = static_cast<uint8_t*>(s.ref.Get());
        if (!base) continue;
        *reinterpret_cast<int32_t*>(base + g_lapsOff) = 0;
        *reinterpret_cast<float*>(base + g_angleOff) = s.angle;
    }
    g_spawners.clear();
}

// Every orb the record held is destroyed; whether that held is read on the next Tick.
void DestroyOrbs() {
    for (const ue_wrap::CachedObjRef& o : g_orbs)
        if (o.Alive()) ue_wrap::engine::DestroyActor(o.Raw());
}

// laps is an int and angle a float, by their reflected types; false (said) when the dump no longer holds.
bool ResolveOrbOffsets(void* cls) {
    std::wstring lapsType, angleType;
    const bool found = R::PropertyOffsetAndClass(cls, L"laps", g_lapsOff, lapsType) &&
                       R::PropertyOffsetAndClass(cls, L"angle", g_angleOff, angleType);
    if (found && g_lapsOff >= 0 && g_angleOff >= 0 && lapsType == L"IntProperty" && angleType == L"FloatProperty")
        return true;
    UE_LOGI("[NB-0] orb BROKEN blackballSpawner_C laps=%ls@%d angle=%ls@%d", lapsType.c_str(), g_lapsOff,
            angleType.c_str(), g_angleOff);
    return false;
}

// The run's end: the answer when there is one (a broken instrument has none), then the done line.
void FinishOrb(const char* answer) {
    if (answer) UE_LOGI("[NB-0] orb native %s instances=%d", answer, g_orbInstances);
    UE_LOGI("[NB-0] orb DONE");
    g_orb = Orb::Done;
    g_orbs.clear();
}

// An orb the destroy did not take is said, then the run ends on the answer.
void ConcludeSpawned() {
    for (const ue_wrap::CachedObjRef& o : g_orbs) {
        if (!o.Alive()) continue;
        UE_LOGE("[NB-0] orb NOT-CONTAINED");
        break;
    }
    FinishOrb("spawned");
}

void TickOrb() {
    if (g_orb == Orb::Done || IsHost() || !coop::npc_adoption::GhostSweepDone()) return;
    if (g_orb == Orb::Verify) {
        ConcludeSpawned();
        return;
    }
    if (!g_orbStartAt) g_orbStartAt = Now();
    void* const cls = OI::ClassByName(L"blackballSpawner_C");
    if (cls && g_orb == Orb::Wait) {
        if (!ResolveOrbOffsets(cls)) {
            FinishOrb(nullptr);
            return;
        }
        g_orb = Orb::Drive;
        UE_LOGI("[NB-0] orb drive start");
    }
    if (!g_orbs.empty()) {
        // The record fired inside the spawn call, before the Blueprint finished the actor; this Tick is after the
        // whole graph run, so each orb is finished and its 1 s delay has not begun to run out.
        DestroyOrbs();
        RestoreSpawners();
        g_orb = Orb::Verify;
        return;
    }
    if (Now() - g_orbStartAt >= kOrbBoundMs) {
        RestoreSpawners();
        FinishOrb("not-spawned");
        return;
    }
    if (cls) g_orbInstances = DriveSpawners(cls);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (ModeOf() == Mode::Off) return;
    g_session = session;
    TryHook();
}

void Tick(coop::net::Session* session) {
    if (ModeOf() != Mode::Orb || !g_hookDone) return;
    g_session = session;
    TickOrb();
}

void OnDisconnect() {
    if (ModeOf() == Mode::Off) return;
    for (int i = 0; i < g_nKeys; ++i) SayKey("total", g_keys[i], /*counted=*/true);
    g_nKeys = 0;
    g_fullSaid = false;
    if (g_orb == Orb::Drive) {
        // An abandoned drive leaves nothing: the spawners are restored and the orbs it recorded destroyed.
        DestroyOrbs();
        RestoreSpawners();
        g_orbs.clear();
        UE_LOGI("[NB-0] orb ABANDONED");
        g_orb = Orb::Wait;
    } else if (g_orb == Orb::Verify) {
        ConcludeSpawned();
    }
    if (g_orb == Orb::Wait) {  // a session that ended before or during the drive starts the next one's clock afresh
        g_orbStartAt = 0;
        g_orbInstances = 0;
    }
    // Cleared until the next session's Install sets it; the record also returns while no session runs.
    g_session = nullptr;
}

}  // namespace coop::dev::native_spawn_probe
