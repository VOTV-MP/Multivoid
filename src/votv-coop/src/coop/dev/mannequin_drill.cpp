// coop/dev/mannequin_drill.cpp -- [dev] the walking mannequin's instrument: the game's own spawn, on request, and a two-peer baseline drill.

#include "coop/dev/mannequin_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_target.h"
#include "coop/dev/dev_gate.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace coop::dev::mannequin_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace WI = ue_wrap::world_identity;
namespace OI = ue_wrap::object_index;

constexpr int kSpawnIdle = 0;
constexpr int kSpawnPrepare = 1;  // requested, the points not shown yet
constexpr int kSpawnWaiting = 2;  // shown, waiting for the spawn
constexpr int kSpawnOk = 3;
constexpr int kSpawnFailed = 4;

constexpr uint64_t kPrepareWaitMs = 500;   // the game's own Delay(0.5) between the two steps
constexpr int kMinTailsAfterPrepare = 3;   // frames rendered with the points shown before the spawn asks
constexpr float kChaseRange = 20000.f;     // the walker's move-to range (prop_wMannequin @92, @166)
constexpr size_t kGameCullAbove = 4;       // the spawner destroys the oldest walker above four
constexpr size_t kMaxListed = 8;           // walkers listed per sample, nearest first
constexpr uint64_t kSampleMs = 1000;
constexpr int kHostSamples = 20;
constexpr int kClientSamplesMax = 120;

std::atomic<int> g_spawn{kSpawnIdle};
std::mutex g_requestMutex;  // every transition INTO kSpawnPrepare, and the two flags below
bool g_skipCall = false;    // the red arm: take the first point without the spawn call
bool g_fromDrill = false;   // the drill's request is not refused at five walkers

bool g_stepSkipCall = false;      // game thread: the request's flags, read once by PrepareStep
bool g_stepFromDrill = false;     // game thread
uint64_t g_dueMs = 0;             // game thread
int g_tailsSincePrepare = 0;      // game thread
uint64_t g_worldGeneration = 0;   // game thread
size_t g_before = 0;              // game thread

// A step ends in a state it chose, through End(): left any other way (a fault unwinding past it)
// the request is failed, so no request stays running for the rest of the process. A step that
// chose its end is done, so a request a button press queued after it is never failed by it.
struct StepGuard {
    int from;
    const char* name;
    bool done = false;
    void End(int state) {
        g_spawn.store(state, std::memory_order_release);
        done = true;
    }
    ~StepGuard() {
        if (done) return;
        int expected = from;
        if (g_spawn.compare_exchange_strong(expected, kSpawnFailed))
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: the %s step did not finish", name);
    }
};

struct Item {
    void* obj;
    float dist;  // from the local player's pawn; -1 when it cannot be read
};

bool ReadWalkerBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
}

bool WalkerPawnValid(void* obj, bool& outValid) {
    if (!obj || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), L"pawn");
    if (off < 0) return false;
    void* target = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(obj) + off);
    outValid = target != nullptr && R::IsLive(target);
    return true;
}

bool InCurrentWorld(void* obj) { return WI::WorldOf(obj) == WI::CurrentWorld(); }

struct ListCtx {
    std::vector<void*>* out;
    void* world;
};

void OnWalker(void* ctx, void* obj, int32_t index) {
    auto* c = static_cast<ListCtx*>(ctx);
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
    if (!R::IsLive(obj)) return;
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;  // the class default, as FindObjectsByClass skips it
    if (WI::WorldOf(obj) != c->world) return;
    c->out->push_back(obj);
}

// The one lister of live walkers in this world: the count and the sampler read the same set.
void ListWalkers(std::vector<void*>& out) {
    out.clear();
    void* cls = OI::ClassByName(L"prop_wMannequin_C");
    if (!cls) return;
    ListCtx c{&out, WI::CurrentWorld()};
    OI::ForEachInstance(cls, OnWalker, &c);
}

size_t CountWalkers() {
    std::vector<void*> v;
    ListWalkers(v);
    return v.size();
}

// The spawn points of this world: a walk of the object array, twice per request and never in a sample.
std::vector<void*> ListPoints() {
    std::vector<void*> out;
    for (void* p : R::FindObjectsByClass(L"wMannequinSpawn_C"))
        if (R::IsLive(p) && InCurrentWorld(p)) out.push_back(p);
    return out;
}

// Each actor with its distance from the local player's pawn, nearest first. With no local pawn, or
// with one location unreadable, the list keeps its order (and the unreadable ones print -1).
std::vector<Item> Measure(const std::vector<void*>& actors) {
    std::vector<Item> items;
    items.reserve(actors.size());
    ue_wrap::FVector me;
    void* local = coop::players::Registry::Get().Local();
    const bool haveLocal = local && E::TryGetActorLocation(local, me);
    bool allReadable = haveLocal;
    for (void* a : actors) {
        float d = -1.f;
        ue_wrap::FVector p;
        if (haveLocal && E::TryGetActorLocation(a, p)) {
            const float dx = p.X - me.X, dy = p.Y - me.Y, dz = p.Z - me.Z;
            d = std::sqrt(dx * dx + dy * dy + dz * dz);
        } else {
            allReadable = false;
        }
        items.push_back({a, d});
    }
    if (allReadable)
        std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.dist < y.dist; });
    return items;
}

bool RequestSpawn(bool skipCall, bool fromDrill) {
    std::lock_guard<std::mutex> lock(g_requestMutex);
    const int s = g_spawn.load(std::memory_order_acquire);
    if (s == kSpawnPrepare || s == kSpawnWaiting) {
        UE_LOGI("mannequin_drill: a spawn is already running");
        return false;
    }
    g_skipCall = skipCall;
    g_fromDrill = fromDrill;
    g_spawn.store(kSpawnPrepare, std::memory_order_release);
    return true;
}

void PrepareStep() {
    StepGuard guard{kSpawnPrepare, "prepare"};
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_stepSkipCall = g_skipCall;
        g_stepFromDrill = g_fromDrill;
    }
    const std::vector<void*> points = ListPoints();
    if (points.empty()) {
        UE_LOGW("[MANNEQUIN-DRILL] FAIL: no spawn point in this world");
        guard.End(kSpawnFailed);
        return;
    }
    if (!g_stepFromDrill && CountWalkers() > kGameCullAbove) {
        UE_LOGW("mannequin_drill: REFUSED -- five walkers are already alive (the game culls above four)");
        guard.End(kSpawnFailed);
        return;
    }
    for (void* point : points) {
        void* fn = R::FindFunction(R::ClassOf(point), L"prepareSpawn");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !ue_wrap::Call(point, f)) {
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: wMannequinSpawn_C.prepareSpawn is not callable");
            guard.End(kSpawnFailed);
            return;
        }
    }
    g_worldGeneration = WI::Generation();
    g_dueMs = GetTickCount64() + kPrepareWaitMs;
    g_tailsSincePrepare = 0;
    UE_LOGI("[MANNEQUIN-DRILL] host: prepared %zu spawn points", points.size());
    guard.End(kSpawnWaiting);
}

void SpawnStep() {
    StepGuard guard{kSpawnWaiting, "spawn"};
    if (WI::Generation() != g_worldGeneration) {
        UE_LOGW("[MANNEQUIN-DRILL] FAIL: the world changed between prepareSpawn and spawn");
        guard.End(kSpawnFailed);
        return;
    }
    std::vector<void*> raw = ListPoints();
    const std::vector<Item> points = Measure(raw);
    g_before = CountWalkers();
    size_t tried = 0;
    for (const Item& it : points) {
        ++tried;
        if (g_stepSkipCall) {
            UE_LOGI("[MANNEQUIN-DRILL] host: red arm -- point %zu of %zu taken without the spawn call, walkers before=%zu",
                    tried, points.size(), g_before);
            guard.End(kSpawnOk);
            return;
        }
        void* fn = R::FindFunction(R::ClassOf(it.obj), L"spawn");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || f.ParamOffset(L"return") < 0) {
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: wMannequinSpawn_C.spawn is not callable");
            guard.End(kSpawnFailed);
            return;
        }
        // False from Call: the dispatch faulted and was absorbed, which is not a refusal.
        if (!ue_wrap::Call(it.obj, f)) {
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: wMannequinSpawn_C.spawn faulted at point %zu", tried);
            guard.End(kSpawnFailed);
            return;
        }
        if (!f.Get<bool>(L"return")) continue;         // refused: on screen in the last 0.2 s
        UE_LOGI("[MANNEQUIN-DRILL] host: spawned at point %zu of %zu, dist=%.0f%s, walkers before=%zu", tried,
                points.size(), it.dist,
                it.dist > kChaseRange ? " (beyond the walker's 20000 chase range)" : "", g_before);
        guard.End(kSpawnOk);
        return;
    }
    UE_LOGW("[MANNEQUIN-DRILL] FAIL: no spawn point accepted (%zu tried)", tried);
    guard.End(kSpawnFailed);
}

// One sample, both halves: the live walkers, nearest eight, what this peer reads of each.
void LogSample(const char* role, int index) {
    std::vector<void*> raw;
    ListWalkers(raw);
    const std::vector<Item> walkers = Measure(raw);
    UE_LOGI("[MANNEQUIN-DRILL] %s: sample %d walkers=%zu", role, index, walkers.size());
    int n = 0;
    for (const Item& it : walkers) {
        if (static_cast<size_t>(n) >= kMaxListed) break;
        ++n;
        // A READ of the key: a drill that minted one would change what the lanes under test see.
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(it.obj);
        ue_wrap::FVector loc{-1.f, -1.f, -1.f};
        if (!E::TryGetActorLocation(it.obj, loc)) loc = ue_wrap::FVector{-1.f, -1.f, -1.f};
        bool pawnValid = false, looking = false, saw = false, angry = false;
        const int pawn = WalkerPawnValid(it.obj, pawnValid) ? (pawnValid ? 1 : 0) : -1;
        const int lookingV = ReadWalkerBool(it.obj, L"looking", looking) ? (looking ? 1 : 0) : -1;
        const int sawV = ReadWalkerBool(it.obj, L"saw", saw) ? (saw ? 1 : 0) : -1;
        const int angryV = ReadWalkerBool(it.obj, L"angry", angry) ? (angry ? 1 : 0) : -1;
        // The slot the host's walker hunts (-1 on a client, which holds none, and before its first read).
        const uint8_t hunted = coop::walker_target::TargetOf(it.obj);
        const int huntedV = hunted == coop::walker_target::kNoTarget ? -1 : static_cast<int>(hunted);
        const int driven = (coop::remote_prop::IsActorUnderAnyDrive(it.obj) ||
                            coop::prop_drive_stream::IsParked(it.obj)) ? 1 : 0;
        UE_LOGI("[MANNEQUIN-DRILL] %s: sample %d walker %d key='%ls' loc=(%.0f,%.0f,%.0f) dist=%.0f pawn=%d "
                "driven=%d looking=%d saw=%d angry=%d target=%d",
                role, index, n, key.c_str(), loc.X, loc.Y, loc.Z, it.dist, pawn, driven, lookingV, sawV, angryV, huntedV);
    }
}

enum class HostStep { WaitClient, AwaitSpawn, Sample, Done };
HostStep g_host = HostStep::WaitClient;
uint64_t g_hostLastMs = 0;
int g_hostSamples = 0;

bool g_clientStarted = false;
uint64_t g_clientLastMs = 0;
int g_clientSamples = 0;

// Some client's join and its join window are over: while the host's late flush is armed a moved
// prop reaches the joiner as the join's own position correction, not through the lane under test.
bool AClientIsSettled(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    }
    return false;
}

void TickHost(coop::net::Session& s, const std::string& mode) {
    switch (g_host) {
    case HostStep::WaitClient:
        if (!AClientIsSettled(s)) return;
        // False: a button request is already running, and its outcome is the one used.
        RequestSpawn(mode == "red", true);
        g_host = HostStep::AwaitSpawn;
        return;
    case HostStep::AwaitSpawn: {
        const int st = g_spawn.load(std::memory_order_acquire);
        if (st == kSpawnFailed) {
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: the spawn request ended without a walker");
            g_host = HostStep::Done;
            return;
        }
        if (st != kSpawnOk) return;
        // The first sample comes a second later, after the object index has taken the walker's birth.
        g_hostLastMs = GetTickCount64();
        g_hostSamples = 0;
        g_host = HostStep::Sample;
        return;
    }
    case HostStep::Sample: {
        const uint64_t now = GetTickCount64();
        if (now - g_hostLastMs < kSampleMs) return;
        g_hostLastMs = now;
        ++g_hostSamples;
        LogSample("host", g_hostSamples);
        if (g_hostSamples == 1 && CountWalkers() <= g_before) {
            UE_LOGW("[MANNEQUIN-DRILL] FAIL: spawned, and the sampler sees no new walker");
            g_host = HostStep::Done;
            return;
        }
        if (g_hostSamples >= kHostSamples) {
            UE_LOGI("[MANNEQUIN-DRILL] host DONE samples=%d", kHostSamples);
            g_host = HostStep::Done;
        }
        return;
    }
    case HostStep::Done:
        return;
    }
}

void TickClient() {
    if (!g_clientStarted) {
        // The predicate behind `rig: READY joined`.
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_clientStarted = true;
        g_clientLastMs = 0;
    }
    if (g_clientSamples >= kClientSamplesMax) return;
    const uint64_t now = GetTickCount64();
    if (g_clientSamples > 0 && now - g_clientLastMs < kSampleMs) return;
    g_clientLastMs = now;
    ++g_clientSamples;
    LogSample("client", g_clientSamples);
}

}  // namespace

void SpawnWalker() {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("mannequin_drill: REFUSED -- a client in a session cannot spawn");
        return;
    }
    RequestSpawn(false, false);
}

void TickFrame() {
    const int s = g_spawn.load(std::memory_order_acquire);
    if (s == kSpawnPrepare) {
        PrepareStep();
    } else if (s == kSpawnWaiting) {
        ++g_tailsSincePrepare;
        if (GetTickCount64() >= g_dueMs && g_tailsSincePrepare >= kMinTailsAfterPrepare) SpawnStep();
    }
}

void Tick(coop::net::Session* session) {
    static const std::string s_mode =
        coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill);
    // The other values are the two-peer walker drill's (coop/dev/mannequin_walker_drill): it spawns through SpawnWalker itself.
    if ((s_mode != "spawn" && s_mode != "red") || !session) return;
    if (coop::roster::LocalIsHost()) TickHost(*session, s_mode);
    else TickClient();
}

void OnDisconnect() {
    g_clientStarted = false;
    g_clientSamples = 0;
    g_clientLastMs = 0;
}

}  // namespace coop::dev::mannequin_drill
