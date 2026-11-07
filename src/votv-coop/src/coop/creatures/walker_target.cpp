// coop/creatures/walker_target.cpp -- see coop/creatures/walker_target.h.

#include "coop/creatures/walker_target.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/death_seam.h"
#include "coop/element/element.h"
#include "coop/creatures/served_player.h"
#include "coop/creatures/walker_graph.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/class_reask.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coop::walker_target {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace DS = coop::element::death_seam;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace OI = ue_wrap::object_index;
namespace PL = coop::players;
namespace R = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace WG = coop::walker_graph;

// The native the walker's Blueprint reads player 0's camera manager through, at the two places it names a player: the range
// test of its hunt and the move toward it. Its hunt graph is the one body every entry of its Blueprint runs through.
UE_FACT_FN(kRowCameraManager, "walker_target", L"GameplayStatics", L"GetPlayerCameraManager", Declared);
constexpr const wchar_t* kGraphName = L"ExecuteUbergraph_prop_wMannequin";
constexpr const wchar_t* kClassName = L"prop_wMannequin_C";
constexpr int            kSites = 2;

// The host holds up to five walkers.
constexpr size_t kMaxWalkers = 8;
// Six: the target-changed list holds the module's own log line, the served record, the walker state sender and the walker drill's
// loss reading, and the graph list its consumers, with room left for one more of either.
constexpr size_t kMaxSubs = 6;

// The host holds up to five walkers and the tick drains every frame, so a full queue is a defect: said once, and the walker
// is not claimed (no retry: a retry is a crutch, the sweep at the class resolve covers only what the class missed).
constexpr size_t kQueueCap = 16;

struct Entry {
    void*                    actor;
    coop::element::ElementId eid;
};

std::mutex          g_queueMutex;  // a leaf: nothing is called under it
Entry               g_queue[kQueueCap];
size_t              g_queueCount = 0;
std::atomic<size_t> g_pending{0};  // g_queueCount, readable without the lock
std::atomic<bool>   g_overflowSaid{false};

// The walker's class and the listing it was found under (ObjectIndex ClassListingId: a class unloaded and loaded again, even at
// the same address, answers a new id). While it is null the lookup is asked again by g_ask: on a class-set move, then each tick
// for its load window (a class finishing its load moves no number, object_index.h:55-58). The atomic is the pointer the
// subscriber compares on whichever thread MarkPropElement runs.
void*                       g_class = nullptr;
uint64_t                    g_listing = 0;
ue_wrap::class_reask::Latch g_ask;
std::atomic<void*>          g_walkerClass{nullptr};

// The subscription is asked for once for the process: a table that was full stays full, so the failure is said once and not retried.
bool g_installAsked = false;

// A held walker: its slot index and serial name the actor, so an entry that outlived its actor is never taken for a walker
// born at the same address. Game thread only.
struct Walker {
    void*   actor;
    int32_t index;
    int32_t serial;
    uint8_t target;
};
Walker            g_walkers[kMaxWalkers];
size_t            g_walkerCount = 0;
bool              g_fullSaid = false;
TargetChangedFn   g_subs[kMaxSubs];
size_t            g_subCount = 0;

// The hook: installed once for the process, armed while a walker is held and its graph is pinned. `g_ubergraph` is what the
// hook's one pointer compare reads; `g_ends` are the offsets the two pinned calls end at.
std::atomic<void*> g_ubergraph{nullptr};
int32_t            g_ends[kSites] = {-1, -1};
void*              g_cameraFn = nullptr;
bool               g_hookInstalled = false;
bool               g_armed = false;
bool               g_endSubscribed = false;

// What the graph subscribers were last told: the pinned graph while a walker is held, else null.
GraphFn g_graphSubs[kMaxSubs];
size_t  g_graphSubCount = 0;
void*   g_graphNotified = nullptr;

void Notify(void* walker, uint8_t slot) {
    for (size_t i = 0; i < g_subCount; ++i) g_subs[i](walker, slot);
}

// The module's own subscriber: every change in the log, the drill's marker (slot -1 is none).
void LogTargetChanged(void* walker, uint8_t slot) {
    UE_LOGI("walker_target: walker %p hunts slot %d", walker, slot == kNoTarget ? -1 : static_cast<int>(slot));
}

// The walker's own reads of "the player" answer its target: its tick tests the player's ragdoll through lib.getMainPlayer and
// its hit lands through the same read, so served_player records the walker against its target's slot (the one record, written
// only here, from the one source of the target). A client's slot is served; the host, no target, a lost target and a walker's
// end withdraw it. A seam that is not ready leaves the walker reading the host, which served_player says once.
void ServeTarget(void* walker, uint8_t slot) {
    if (slot == kNoTarget) coop::served_player::Unserve(walker);
    else coop::served_player::Serve(walker, slot, coop::served_player::kMainPlayer);
}

Walker* Find(void* actor) {
    for (size_t i = 0; i < g_walkerCount; ++i)
        if (g_walkers[i].actor == actor) return &g_walkers[i];
    return nullptr;
}

void OnCameraManagerPost(void* context, void* source, void* result);
void Rearm();

// A held walker's entry goes (its end of play, or an entry found to have outlived its actor): the hook is disarmed with the last,
// and a walker that had a target is told it has none, which withdraws its served record (ServeTarget) and its other subscribers'.
void Forget(Walker* w) {
    const uint8_t was = w->target;
    void* const   actor = w->actor;
    *w = g_walkers[--g_walkerCount];
    Rearm();
    if (was != kNoTarget) Notify(actor, kNoTarget);
}

// Armed while a walker is held and its graph is pinned: with none the hook does nothing at all, and the forwarder pays its one
// load and branch. The graph subscribers hear the same state, so their own hooks follow it.
void Rearm() {
    void* const graph = g_ubergraph.load(std::memory_order_relaxed);
    const bool  want = g_walkerCount > 0 && graph != nullptr;
    void* const held = want ? graph : nullptr;
    if (held != g_graphNotified) {
        g_graphNotified = held;
        for (size_t i = 0; i < g_graphSubCount; ++i) g_graphSubs[i](held);
    }
    if (!g_hookInstalled || want == g_armed) return;
    if (UH::SetArmed(g_cameraFn, &OnCameraManagerPost, want)) g_armed = want;
}

// A client's body: its puppet once it has a pose.
void* PuppetBody(uint8_t slot) {
    coop::RemotePlayer* rp = PL::Registry::Get().Puppet(slot);
    return (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
}

// The host is always a player the Blueprint can hunt; a client counts while its puppet lives (LifeOf).
bool Alive(uint8_t slot) {
    return slot == PL::kPeerIdHost || PL::Registry::Get().LifeOf(slot) == PL::PuppetLife::Alive;
}

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Each slot as the walker's read at `at` finds it, for the rule (walker_target_rule.h). The host's body is the camera manager the
// Blueprint itself read, the object its own range test measures to; a client's is its puppet once it has a pose. A body that does
// not read gives a negative distance.
size_t ViewPlayers(const ue_wrap::FVector& at, void* camera, PlayerView (&out)[PL::kMaxPeers]) {
    size_t n = 0;
    for (uint8_t slot = 0; slot < PL::kMaxPeers; ++slot) {
        void*            body = slot == PL::kPeerIdHost ? camera : PuppetBody(slot);
        ue_wrap::FVector p{};
        const bool       read = body && E::TryGetActorLocation(body, p);
        out[n++] = PlayerView{slot, Alive(slot), read ? Distance(at, p) : -1.f};
    }
    return n;
}

// The post hook on GameplayStatics.GetPlayerCameraManager, game thread inside the engine's own call. The first thing it does
// is one compare of the calling frame's function with the walker's graph, which every other caller of the native fails.
void OnCameraManagerPost(void* /*context*/, void* source, void* /*result*/) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    void* const           graph = g_ubergraph.load(std::memory_order_relaxed);
    if (frame.function != graph || !graph) return;
    if (!GT::IsGameThread() || (frame.codeOffset != g_ends[0] && frame.codeOffset != g_ends[1])) return;
    Walker* w = Find(source);
    void*   slotResult = UH::CurrentResult();
    if (!w || !slotResult) return;
    if (R::SlotSerial(w->index) != w->serial || R::InternalIndexOf(source) != w->index) {
        Forget(w);  // an entry that outlived its actor: its served record and its subscribers go with it
        return;
    }
    void* const      camera = *static_cast<void**>(slotResult);
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(source, at)) return;
    const uint8_t was = w->target;
    PlayerView views[PL::kMaxPeers];
    const size_t n = ViewPlayers(at, camera, views);
    w->target = DecideTarget(was, views, n);
    const uint8_t now = w->target;
    if (now != PL::kPeerIdHost) {
        // An absent body leaves the read as written: the Blueprint reads through what this returns, and a null would
        // fault its next read.
        if (void* body = PuppetBody(now)) *static_cast<void**>(slotResult) = body;
    }
    if (now != was) Notify(source, now);
}

void InstallHook() {
    if (g_hookInstalled) return;
    const ue_wrap::facts::Resolved native = ue_wrap::facts::Resolve(kRowCameraManager);
    g_cameraFn = native.Fn();
    if (!native || !UH::InstallPostHook(native, &OnCameraManagerPost, /*armed=*/false)) {
        UE_LOGE("walker_target: the camera manager seam did not install -- a walker hunts only the host for the rest of "
                "this process");
        return;
    }
    g_hookInstalled = true;
}

// The walker's calls of the native, found in its bytecode: exactly two, or the hunt is left to the Blueprint. A pin is
// the graph and the end offsets of the two calls, so a build that moved them is read afresh and no offset is written here.
void PinSites(void* cls) {
    g_ubergraph.store(nullptr, std::memory_order_relaxed);
    void* graph = R::FindFunction(cls, kGraphName);
    void* native = ue_wrap::facts::Resolve(kRowCameraManager).Fn();
    WG::Calls calls;
    if (!WG::FindCalls(graph, native, calls)) {
        UE_LOGE("walker_target: the walker's graph or its camera manager call did not resolve -- a walker hunts only the "
                "host in this world");
        return;
    }
    if (calls.found != kSites) {
        UE_LOGE("walker_target: %d call(s) of the camera manager in the walker's graph, %d are pinned -- a walker hunts only "
                "the host in this world", calls.found, kSites);
        return;
    }
    g_ends[0] = calls.ends[0];
    g_ends[1] = calls.ends[1];
    g_ubergraph.store(graph, std::memory_order_relaxed);
    UE_LOGI("walker_target: the walker reads its player at graph offsets %d and %d", g_ends[0], g_ends[1]);
}

// A held walker ends, by the death seam: its entry goes, and the hook is disarmed with the last.
void OnWalkerEnd(const DS::ActorEnd& end) {
    if (Walker* w = Find(end.actor)) Forget(w);
}

// Hold `actor`: its target is chosen at its next read. A walker held already keeps its target.
void Hold(void* actor) {
    const int32_t index = R::InternalIndexOf(actor);
    const int32_t serial = R::SlotSerial(index);
    if (Walker* w = Find(actor)) {
        if (w->index == index && w->serial == serial) return;
        const uint8_t was = w->target;
        *w = Walker{actor, index, serial, kNoTarget};  // a new walker at a dead one's address: the dead one's record goes
        if (was != kNoTarget) Notify(actor, kNoTarget);
        return;
    }
    if (g_walkerCount >= kMaxWalkers) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("walker_target: %zu walkers are held -- another is not hunted (said once)", kMaxWalkers);
        }
        return;
    }
    g_walkers[g_walkerCount++] = Walker{actor, index, serial, kNoTarget};
    Rearm();
}

// MarkPropElement's callback, any thread: a non-walker is rejected by the class pointer, a walker is queued.
void OnExpressed(void* actor, coop::element::ElementId eid) {
    void* const cls = g_walkerClass.load(std::memory_order_acquire);
    if (!cls || !actor || R::ClassOf(actor) != cls) return;
    bool full = false;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        if (g_queueCount >= kQueueCap) {
            full = true;
        } else {
            g_queue[g_queueCount++] = Entry{actor, eid};
            g_pending.store(g_queueCount, std::memory_order_release);
        }
    }
    // The log is written after the leaf mutex is let go: nothing is called under it.
    if (full && !g_overflowSaid.exchange(true))
        UE_LOGW("walker_target: the birth queue is full (%zu); a walker is not claimed", kQueueCap);
}

// Claim `actor` if it still holds `eid` and is alive: the registry's reverse is the test for an actor that died in the queue.
// A claimed walker is held for the hunt.
void ClaimWalker(void* actor, coop::element::ElementId eid) {
    if (coop::element::Registry::Get().EidForActor(actor) != eid || !R::IsLive(actor)) return;
    // The mannequin drill's red: the walker is held for the hunt and not claimed into the drive stream, so its copy is not moved
    // and the drill's follow step must FAIL.
    static const bool s_redNoClaim = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_drive_claim";
    if (!s_redNoClaim) coop::prop_drive_host::Claim(actor, "walker");
    Hold(actor);
}

// The class resolved: pin its graph, install the hook and the end subscription (the class is loaded, so each resolves), and
// on a new class claim the walkers that already hold an eid.
void ResolveClass() {
    void* const cls = AC::WalkerClass();
    if (!cls) return;
    g_class = cls;
    g_listing = OI::ClassListingId(cls);
    g_walkerClass.store(cls, std::memory_order_release);
    PinSites(cls);
    InstallHook();
    if (!g_endSubscribed) {
        g_endSubscribed = DS::Install() && DS::SubscribeClass(kClassName, &OnWalkerEnd);
        if (!g_endSubscribed) UE_LOGE("walker_target: the end of play of a walker is not heard -- its target is kept to its slot");
    }
    std::vector<void*> alive;
    AC::ListWalkers(alive);
    for (void* w : alive) {
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(w);
        if (eid != coop::element::kInvalidId) ClaimWalker(w, eid);
    }
    Rearm();
}

// The class again when its listing changed (an unload) and, while it is null, when g_ask says: a class set move and the load
// window after it, then nothing until the next move, so a class that never loads is not asked every tick.
void RefreshClass() {
    if (g_class && OI::ClassListingId(g_class) != g_listing) {
        g_class = nullptr;
        g_walkerClass.store(nullptr, std::memory_order_release);
        g_ubergraph.store(nullptr, std::memory_order_relaxed);
        g_ask.Reset();
        Rearm();
    }
    if (g_class) return;
    if (!g_ask.Due()) return;
    ResolveClass();
}

}  // namespace

void Install(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("walker_target::Install");
    if (session.role() != coop::net::Role::Host || g_installAsked) return;
    g_installAsked = true;
    SubscribeTargetChanged(&LogTargetChanged);
    SubscribeTargetChanged(&ServeTarget);
    const bool expressed = coop::prop_element_tracker::SubscribeHostPropExpressed(&OnExpressed);
    if (expressed) UE_LOGI("walker_target: the host's walkers are claimed into the drive stream at their eid mint");
    else UE_LOGE("walker_target: the prop lane's subscriber table is full -- the host's walker is not claimed");
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    RefreshClass();
    if (g_pending.load(std::memory_order_acquire) == 0) return;
    Entry   taken[kQueueCap];
    size_t  n = 0;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        for (; n < g_queueCount; ++n) taken[n] = g_queue[n];
        g_queueCount = 0;
        g_pending.store(0, std::memory_order_release);
    }
    for (size_t i = 0; i < n; ++i) ClaimWalker(taken[i].actor, taken[i].eid);
}

void* ClientBody(uint8_t slot) { return PuppetBody(slot); }

uint8_t TargetOf(void* walker) {
    const Walker* w = Find(walker);
    return w ? w->target : kNoTarget;
}

bool SubscribeTargetChanged(TargetChangedFn fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_subCount; ++i)
        if (g_subs[i] == fn) return false;
    if (g_subCount >= kMaxSubs) return false;
    g_subs[g_subCount++] = fn;
    return true;
}

bool SubscribeGraph(GraphFn fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_graphSubCount; ++i)
        if (g_graphSubs[i] == fn) return false;
    if (g_graphSubCount >= kMaxSubs) return false;
    g_graphSubs[g_graphSubCount++] = fn;
    // A subscriber that arrives while a walker is held is told at once.
    if (g_graphNotified) fn(g_graphNotified);
    return true;
}

void ForEachHeld(HeldFn fn, void* ctx) {
    for (size_t i = 0; i < g_walkerCount; ++i) {
        const Walker& w = g_walkers[i];
        if (R::SlotSerial(w.index) != w.serial || R::InternalIndexOf(w.actor) != w.index) continue;
        fn(ctx, w.actor, w.target);
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot == PL::kPeerIdHost || slot == kNoTarget) return;
    // The mannequin drill's red: a leaving player's walkers are not told, so they keep the hunt of a slot nobody holds and the
    // drill's loss step must FAIL.
    static const bool s_redNoLoss = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_loss";
    if (s_redNoLoss) return;
    for (size_t i = 0; i < g_walkerCount; ++i) {
        if (g_walkers[i].target != slot) continue;
        g_walkers[i].target = kNoTarget;
        Notify(g_walkers[i].actor, kNoTarget);
    }
}

void OnDisconnect() {
    g_walkerCount = 0;
    g_fullSaid = false;
    g_ubergraph.store(nullptr, std::memory_order_relaxed);
    Rearm();
    g_walkerClass.store(nullptr, std::memory_order_release);
    g_class = nullptr;
    g_listing = 0;
    g_ask.Reset();
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queueCount = 0;
    g_pending.store(0, std::memory_order_release);
    g_overflowSaid.store(false, std::memory_order_relaxed);
}

}  // namespace coop::walker_target
