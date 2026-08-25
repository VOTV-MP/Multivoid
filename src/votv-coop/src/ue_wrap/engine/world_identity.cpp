// ue_wrap/engine/world_identity.cpp -- see the header for WHY.

#include "ue_wrap/engine/world_identity.h"

#include "ue_wrap/core/cached_obj_ref.h"  // the drill measures the SHIPPED predicate
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>  // GetTickCount64, GetEnvironmentVariableA

#include <atomic>

namespace ue_wrap::world_identity {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// ---- published state -------------------------------------------------------
// Written on the game thread, read from anywhere. The pointer is an IDENTITY: no
// reader dereferences it, so publishing a world that dies a microsecond later is
// harmless -- the comparison simply stops matching, which is the correct answer.
std::atomic<void*>    g_currentWorld{nullptr};
std::atomic<uint32_t> g_generation{1};   // 0 is reserved for "never stamped"
std::atomic<bool>     g_degraded{false};
std::atomic<WorldKind> g_worldKind{WorldKind::Unknown};

// The gameplay map's name, as a SUBSTRING -- and the substring is a DELIBERATE WIDENING, not a
// case dodge. (An earlier version of this comment said it existed because the live UWorld is
// named "Untitled_1" with a capital U; that reason is false -- `NameContains` compares with
// `_wcsnicmp` and is case-insensitive already, so `NameEquals(name, P::name::GameplayLevel)`
// would match. It is kept as a substring because it is what the reaper matched before this
// module took the question over, and switching to the exact name would reclassify every OTHER
// `untitled_*` map -- the RE census names sixteen more (untitled_47/55/80/.../211) -- from
// Gameplay to Other, i.e. from "reap here" to "flee from here". None is a travel target in
// the 637-dump census, so the two forms are behaviourally identical TODAY; the exact-name form
// is the better one and wants its own verified change, not a silent one riding this commit.
// `P::name::GameplayLevel` remains the authoritative spelling on the version surface.
//
// Everything that is NOT this is `Other` -- the menu, preLoad and the three tutorial maps are
// the ones mainGamemode's own level array enumerates; that array is the TRAVEL set, not the
// full map list, which is why this is written as a complement rather than as a list.
constexpr const wchar_t* kGameplayWorldSubstr = L"ntitled";

// ---- resolution (name-driven; the version surface, per docs/VERSION_MIGRATION) --
bool    g_resolved         = false;
void*   g_levelCls         = nullptr;
void*   g_worldCls         = nullptr;
int32_t g_owningWorldOff   = -1;   // ULevel::OwningWorld
int32_t g_localPlayersOff  = -1;   // UGameInstance::LocalPlayers (TArray<ULocalPlayer*>)
int32_t g_playerCtrlOff    = -1;   // UPlayer::PlayerController

// The GameInstance, cached as a raw (ptr, index) pair rather than a CachedObjRef --
// CachedObjRef::Alive() consults THIS module, so using one here would recurse.
void*   g_gameInstance     = nullptr;
int32_t g_gameInstanceIdx  = -1;

// UE4.27 TArray<T> == { T* Data; int32 Num; int32 Max; }.
struct ArrayHeader {
    void*   data;
    int32_t num;
    int32_t max;
};

// The class of `obj`, or nullptr. Split out so the outer climb reads once.
inline void* ClassOfSafe(void* o) { return o ? R::ClassOf(o) : nullptr; }

// Resolve the three property offsets + two classes. Everything here is name-driven so
// a game recook is a MISS (loud, degraded) rather than a wrong read at a stale hard
// offset.
//
// ASK THE NATIVE DECLARING CLASSES, NOT THE GAME'S BLUEPRINT SUBCLASSES. Measured
// 2026-08-23: the first cut asked `mainGameInstance_C` for `LocalPlayers` and got -1,
// because the very first CachedObjRef::Set of the process runs during BOOT -- before
// any BlueprintGeneratedClass has loaded. `UGameInstance`, `ULocalPlayer`, `ULevel`
// and `UWorld` are native and registered at static-init, so they are answerable from
// the first instruction we run. (FindPropertyOffset climbs the SuperStruct chain, so
// the subclass would ALSO have worked -- once it existed. That "once" is the bug.)
//
// AND IT RETRIES. A one-shot latch during boot is how a transient miss becomes a
// permanent one; the same shape as the `activeInterface` negative latch documented in
// input_owner.cpp. We only latch on FULL success.
void EnsureResolved() {
    if (g_resolved) return;
    // Each attempt costs FindClass walks, and CachedObjRef::Alive() is a hot path, so
    // an unresolved state must not re-walk at the caller's rate.
    static unsigned long long sNextAttemptMs = 0;
    static unsigned long long sFirstAttemptMs = 0;
    const unsigned long long now = ::GetTickCount64();
    if (now < sNextAttemptMs) return;
    sNextAttemptMs = now + 1000;
    if (sFirstAttemptMs == 0) sFirstAttemptMs = now;

    if (!g_levelCls) g_levelCls = R::FindClass(L"Level");
    if (!g_worldCls) g_worldCls = R::FindClass(P::name::WorldClass);
    if (g_levelCls && g_owningWorldOff < 0)
        g_owningWorldOff = R::FindPropertyOffset(g_levelCls, L"OwningWorld");
    if (g_localPlayersOff < 0) {
        if (void* giCls = R::FindClass(L"GameInstance"))
            g_localPlayersOff = R::FindPropertyOffset(giCls, L"LocalPlayers");
    }
    if (g_playerCtrlOff < 0) {
        // PlayerController is declared on UPlayer; ULocalPlayer derives from it.
        if (void* lpCls = R::FindClass(L"LocalPlayer"))
            g_playerCtrlOff = R::FindPropertyOffset(lpCls, L"PlayerController");
    }

    if (g_levelCls && g_worldCls && g_owningWorldOff >= 0 && g_localPlayersOff >= 0 &&
        g_playerCtrlOff >= 0) {
        g_resolved = true;
        LogResolutionStateOnce();
        return;
    }
    // Still incomplete. Report ONCE, and only after a grace period -- during boot an
    // incomplete answer is normal, and crying recook-break at t+0 would make the one
    // line that matters unreadable. Retries continue after the report (slowly), so a
    // genuinely late registration still recovers.
    if (now - sFirstAttemptMs >= 30000) {
        LogResolutionStateOnce();
        sNextAttemptMs = now + 30000;
    }
}

// The GameInstance, revalidated by SLOT (never by dereferencing a possibly-freed
// pointer). It is process-immortal in practice; the revalidation is the same
// belt-and-braces `engine.cpp` uses for its own world context.
void* GameInstance() {
    if (g_gameInstance && !R::IsLiveByIndex(g_gameInstance, g_gameInstanceIdx)) {
        g_gameInstance = nullptr;
        g_gameInstanceIdx = -1;
    }
    if (!g_gameInstance) {
        // One GUObjectArray walk, then cached for the process lifetime. This is the
        // only walk in this module and it does not repeat in steady state.
        g_gameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        g_gameInstanceIdx = g_gameInstance ? R::InternalIndexOf(g_gameInstance) : -1;
    }
    return g_gameInstance;
}

// The local PlayerController the ENGINE currently owns: GameInstance ->
// LocalPlayers[0] -> PlayerController. ULocalPlayer outlives world travel (it is
// outered to the GameInstance), and the engine repoints its PlayerController field
// at each new world's controller -- which is exactly the travel signal a
// liveness test cannot see.
void* CurrentPlayerController_() {
    if (g_localPlayersOff < 0 || g_playerCtrlOff < 0) return nullptr;
    void* gi = GameInstance();
    if (!gi) return nullptr;
    const auto* arr = reinterpret_cast<const ArrayHeader*>(
        reinterpret_cast<const uint8_t*>(gi) + g_localPlayersOff);
    if (!arr->data || arr->num <= 0) return nullptr;
    void* lp = *reinterpret_cast<void* const*>(arr->data);  // LocalPlayers[0]
    if (!lp) return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(lp) +
                                           g_playerCtrlOff);
}

// Refresh cadence for the memoised current world. 100 ms is two orders below the
// multi-second cost of a world transition and two orders above the per-frame rate at
// which Alive() is asked, so the read is effectively free and the staleness is
// invisible against what it measures.
constexpr unsigned long long kRefreshMs = 100;

void RefreshOnGameThread_() {
    // Re-entrancy brake. This path calls reflection (FindClass / FindObjectByClass),
    // reflection caches UClass pointers through CachedObjRef, and CachedObjRef::Alive()
    // calls CurrentWorld() -> here. One nested level is harmless but pointless; a loop
    // is not. Cheap belt beside the structural fix in WorldOf().
    static thread_local bool tInRefresh = false;
    if (tInRefresh) return;
    static unsigned long long sNextMs = 0;
    const unsigned long long now = ::GetTickCount64();
    if (now < sNextMs) return;
    sNextMs = now + kRefreshMs;
    tInRefresh = true;
    struct Guard { ~Guard() { tInRefresh = false; } } _g;

    EnsureResolved();
    void* pc = CurrentPlayerController_();
    // WorldOf dereferences `pc` and then climbs its Outer chain, so `pc` is validated
    // FIRST -- and by the fresh-pointer contract (bare IsLive), which is legitimate
    // here precisely because `pc` was read out of the engine's own field a few
    // instructions ago rather than cached across tasks. Without this the refresh was
    // an unguarded multi-object deref at 10 Hz, forever, INCLUDING through world
    // teardown; it survived only because UE nulls strong UPROPERTY references at GC,
    // which is a property we were relying on without saying so (audit 2026-08-23).
    void* world = (pc && R::IsLive(pc)) ? WorldOf(pc) : nullptr;

    // Classify HERE, where `world` is a pointer the engine handed us microseconds ago, not
    // a cached one. This is the whole reason the answer lives in this module: naming a world
    // requires dereferencing it, and every consumer is forbidden to. `IsLive` is the same
    // fresh-pointer contract the `pc` read above uses.
    WorldKind kind = WorldKind::Unknown;
    if (world && R::IsLive(world)) {
        kind = R::NameContains(R::NameOf(world), kGameplayWorldSubstr) ? WorldKind::Gameplay
                                                                      : WorldKind::Other;
    }
    g_worldKind.store(kind, std::memory_order_relaxed);

    void* prev = g_currentWorld.exchange(world, std::memory_order_relaxed);
    if (prev != world) {
        g_generation.fetch_add(1, std::memory_order_relaxed);
        // The edge, at INFO: this is the line that makes a future "why did my cache
        // survive a travel" question one grep instead of one more hands-on round.
        UE_LOGI("world_identity: current world %p -> %p (gen=%u, pc=%p)", prev, world,
                g_generation.load(std::memory_order_relaxed), pc);
    }
}

}  // namespace

void LogResolutionStateOnce() {
    // The FLAG is refreshed on every call; only the LOG LINE is once. Latching both
    // together is how a transient boot-window miss would be reported as permanent
    // even after a later retry succeeded.
    // The CLASSES belong in this predicate too, not just the offsets: `WorldOf()` rejects
    // unconditionally on `g_levelCls`/`g_worldCls`, so a class-name break would leave every
    // world term dead while this line reported HEALTH -- a log that actively asserts the
    // opposite of the truth is worse than silence (audit 2026-08-25).
    const bool bad = (g_owningWorldOff < 0 || g_localPlayersOff < 0 || g_playerCtrlOff < 0 ||
                      g_levelCls == nullptr || g_worldCls == nullptr);
    g_degraded.store(bad, std::memory_order_relaxed);
    static bool sLogged = false;
    if (sLogged) return;
    sLogged = true;
    if (bad) {
        // A permanent negative latch with no diagnostic is how a recook silently
        // brings the 44-second stale-pawn window back. Name every term so the log
        // says WHICH one moved.
        UE_LOGE("world_identity: DEGRADED -- ULevel::OwningWorld=%d "
                "UGameInstance::LocalPlayers=%d UPlayer::PlayerController=%d "
                "(-1 = not found; ULevel=%p UWorld=%p, null = the CLASS itself did not "
                "resolve). Every world-currency term in the tree now fails "
                "OPEN, i.e. back to liveness-only caches and the 2026-08-23 "
                "stale-cross-world-pawn class of bug. A game recook that renamed one "
                "of these fields looks exactly like this.",
                g_owningWorldOff, g_localPlayersOff, g_playerCtrlOff, g_levelCls, g_worldCls);
    } else {
        UE_LOGI("world_identity: resolved OwningWorld=+0x%X LocalPlayers=+0x%X "
                "PlayerController=+0x%X", g_owningWorldOff, g_localPlayersOff,
                g_playerCtrlOff);
    }
}

// PURE READ -- deliberately does NOT call EnsureResolved(), and that is load-bearing
// re-entrancy, not laziness. `CachedObjRef::Set()` calls this, and `reflection.cpp`
// calls `Set()` from `PrimeClassWalk` **while holding `g_classCacheMu`**
// (`reflection.cpp:408-412`), a non-recursive std::mutex. So nothing reachable from
// here may take that mutex. EnsureResolved() transitively can -- not through
// `R::FindClass` (which takes no lock; an earlier revision of this comment named the
// wrong function and an audit caught it) but through `GameInstance()` ->
// `FindObjectByClass` -> `BeginClassWalk`/`PrimeClassWalk`. Resolution therefore
// belongs to the refresh path, which is entered from Alive() rather than Set() and so
// is never inside that lock. Before resolution completes this answers nullptr, i.e.
// "no world term", i.e. exactly the pre-2026-08-23 behaviour for the handful of
// objects stamped during the boot window (see the residual note in Degraded()).
void* WorldOf(void* obj) {
    if (!obj) return nullptr;
    if (!g_levelCls || !g_worldCls || g_owningWorldOff < 0) return nullptr;
    // Bounded climb. An actor is Outered to its ULevel (1 step); a component to its
    // actor (2); a nested subobject a little deeper. 8 is far past anything the
    // engine builds and makes a corrupted Outer ring terminate instead of spin.
    void* o = obj;
    for (int depth = 0; o && depth < 8; ++depth) {
        void* cls = ClassOfSafe(o);
        if (cls == g_worldCls) return o;            // a UWorld answers for itself
        if (cls == g_levelCls) {
            return *reinterpret_cast<void* const*>(
                reinterpret_cast<const uint8_t*>(o) + g_owningWorldOff);
        }
        o = R::OuterOf(o);
    }
    // Not world-scoped: a UClass, a UFunction, a CDO, a cooked asset, the
    // GameInstance. Their Outer chain reaches a UPackage and stops. nullptr here
    // means "this object has no world term", NOT "the lookup failed" -- see
    // Degraded() for the failure case.
    return nullptr;
}

void* CurrentWorld() {
    // Only the game thread may walk engine memory; everyone else reads the publish.
    if (ue_wrap::game_thread::IsGameThread()) RefreshOnGameThread_();
    return g_currentWorld.load(std::memory_order_relaxed);
}

uint32_t Generation() { return g_generation.load(std::memory_order_relaxed); }

WorldKind CurrentWorldKind() {
    // Same shape as CurrentWorld(): the game thread drives the refresh, everyone else reads
    // the publish. Keeping the drive here means a consumer that only ever asks for the KIND
    // still keeps the memo warm.
    if (ue_wrap::game_thread::IsGameThread()) RefreshOnGameThread_();
    return g_worldKind.load(std::memory_order_relaxed);
}

bool Degraded() { return g_degraded.load(std::memory_order_relaxed); }

// ---- [dev] the instrument --------------------------------------------------
//
// This exists because the design that consumes this module rests on ONE unmeasured
// premise: that `LocalPlayers[0]->PlayerController` actually MOVES at a solo
// quit-to-menu. If it kept pointing at the dead world's controller the whole term
// would be a no-op for exactly the window it targets. Written before the run, with
// the falsifier stated: PASS = during a menu window after solo play, this module's
// world differs from WorldOf(the pawn the registry still hands out).
void TickProbe(void* localPawnForCompare) {
    static int sOn = -1;
    if (sOn == -1) {
        char v[8]{};
        sOn = (::GetEnvironmentVariableA("VOTVCOOP_WORLD_ID_PROBE", v, sizeof(v)) > 0 &&
               v[0] == '1') ? 1 : 0;
    }
    if (sOn != 1) return;
    if (!ue_wrap::game_thread::IsGameThread()) return;
    static unsigned long long sNext = 0;
    const unsigned long long now = ::GetTickCount64();
    if (now < sNext) return;
    sNext = now + 1000;

    EnsureResolved();
    void* pc  = CurrentPlayerController_();
    void* wA  = CurrentWorld();                       // candidate A: the immortal chain
    void* wB  = R::FindObjectByClass(P::name::WorldClass);  // candidate B: what the reaper uses

    // Candidate B's ambiguity, measured rather than asserted: how many live Worlds
    // exist right now? If a dying world lingers in the array, FindObjectByClass
    // answers whichever is indexed first -- which is why B is not the owner here.
    int liveWorlds = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || R::ClassOf(o) != g_worldCls) continue;
        if (!R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        ++liveWorlds;
    }
    // The verdict field. `pawnWorld` is the world of whatever the registry still
    // believes is the local pawn; STALE=1 is the storm condition -- a pawn from a
    // world that is no longer current, which today's liveness-only cache cannot see.
    void* pawnWorld = localPawnForCompare ? WorldOf(localPawnForCompare) : nullptr;
    const int stale = (localPawnForCompare && pawnWorld && wA && pawnWorld != wA) ? 1 : 0;

    // ---- THE DRILL ----------------------------------------------------------
    // The fields above read WorldOf() LIVE off the pawn every sample, which is NOT
    // the shipped predicate: CachedObjRef stamps at Set() and never reads the object
    // again. Measuring the live read would leave the SHIPPED path having never been
    // observed reject anything -- and the storm does not reproduce on this machine
    // (the dead pawn purges in ~6 s here versus 44 s in the field), so there is no
    // natural occasion for it to fire.
    //
    // So: latch the pawn through the real CachedObjRef, ONCE, while it is healthy,
    // and then report the shipped verdict beside the raw slot liveness. Reading
    // `latchAlive=0` while `slotLive=1` is the fix working -- the object is still
    // there and the world term is what rejected it. It cannot re-stamp after the
    // travel because the registry stops handing a pawn out at all.
    static CachedObjRef sLatch;
    static void* sLatchPtr = nullptr;
    if (localPawnForCompare && localPawnForCompare != sLatchPtr) {
        sLatch.Set(localPawnForCompare);
        sLatchPtr = localPawnForCompare;
    }
    const int slotLive = (sLatch.Raw() && R::IsLiveByIndex(sLatch.Raw(), sLatch.Idx())) ? 1 : 0;
    const int latchAlive = sLatch.Alive() ? 1 : 0;

    UE_LOGI("world_id_probe: pc=%p A(chain)=%p B(findfirst)=%p liveWorlds=%d gen=%u "
            "degraded=%d pawn=%p pawnWorld=%p STALE=%d | latch=%p stamp=%p slotLive=%d "
            "latchAlive=%d", pc, wA, wB, liveWorlds,
            g_generation.load(std::memory_order_relaxed), Degraded() ? 1 : 0,
            localPawnForCompare, pawnWorld, stale,
            sLatch.Raw(), sLatch.StampedWorld(), slotLive, latchAlive);

    // ---- THE NEGATIVE CONTROL ----------------------------------------------
    // Everything above can only show the predicate ACCEPTING. On this machine the
    // dead pawn's GUObjectArray slot dies within a second of the travel, so the
    // liveness term rejects first and the world term is never the deciding one --
    // i.e. the shipped predicate would ship having never been observed to reject
    // anything, which is indistinguishable from a term that is wired up wrong.
    // (The field case it exists for held the slot LIVE for 44 s; that timing is
    // GC-dependent and not reproducible here.)
    //
    // So force it: hold the object, the slot and the serial constant, change ONLY the
    // current world, and require the verdict to flip. Shown RED by construction --
    // if the assert below ever prints PASS with latchAlive still 1, the world term is
    // not participating in Alive() at all.
    static int sDrill = -1;
    if (sDrill == -1) {
        char v[8]{};
        sDrill = (::GetEnvironmentVariableA("VOTVCOOP_WORLD_ID_DRILL", v, sizeof(v)) > 0 &&
                  v[0] == '1') ? 1 : 0;
    }
    if (sDrill == 1 && slotLive == 1 && latchAlive == 1 && sLatch.StampedWorld()) {
        static int sDrillsRun = 0;
        if (sDrillsRun < 3) {
            ++sDrillsRun;
            void* const real = g_currentWorld.load(std::memory_order_relaxed);
            // A sentinel that is not any UWorld and is never dereferenced (the whole
            // module treats these as comparison tokens).
            void* const poison =
                reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADD00DDEADD00Dull));
            struct Restore {
                void* v;
                ~Restore() { g_currentWorld.store(v, std::memory_order_relaxed); }
            } _r{real};
            g_currentWorld.store(poison, std::memory_order_relaxed);
            const int poisonedAlive = sLatch.Alive() ? 1 : 0;
            UE_LOGI("world_id_drill: stamp=%p real=%p poisoned=%p -> slotLive=%d "
                    "aliveBefore=1 alivePoisoned=%d  [%s]",
                    sLatch.StampedWorld(), real, poison, slotLive, poisonedAlive,
                    poisonedAlive == 0 ? "PASS -- the world term rejects on its own"
                                       : "FAIL -- world term NOT participating in Alive()");
        }
    }
}

}  // namespace ue_wrap::world_identity
