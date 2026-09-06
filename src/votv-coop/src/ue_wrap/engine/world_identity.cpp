// ue_wrap/engine/world_identity.cpp -- see ue_wrap/engine/world_identity.h.

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

// The published state: written on the game thread, read from anywhere. The pointer is an
// identity, never dereferenced by a reader, so publishing a world that dies a microsecond
// later is harmless; the comparison stops matching, which is the correct answer.
std::atomic<void*>    g_currentWorld{nullptr};
std::atomic<uint32_t> g_generation{1};   // 0 is reserved for "never stamped"
std::atomic<bool>     g_degraded{false};
std::atomic<WorldKind> g_worldKind{WorldKind::Unknown};

// The gameplay map's name as a substring, a deliberate widening: NameContains is
// case-insensitive already, so the exact name would match, but the substring is what the
// reaper matched before this module took the question over, and the exact name would
// reclassify the other untitled maps from gameplay to other, from reap-here to
// flee-from-here; none is a travel target, so the two forms behave the same today, and the
// exact form wants its own verified change. The authoritative spelling stays on the version
// surface. Everything else is Other: the menu, the preload and the tutorial maps, which the
// gamemode's own level array enumerates as the travel set, not the full map list, hence a
// complement rather than a list.
constexpr const wchar_t* kGameplayWorldSubstr = L"ntitled";

// Resolution, name-driven: the version surface.
bool    g_resolved         = false;
void*   g_levelCls         = nullptr;
void*   g_worldCls         = nullptr;
int32_t g_owningWorldOff   = -1;   // ULevel::OwningWorld
int32_t g_localPlayersOff  = -1;   // UGameInstance::LocalPlayers (TArray<ULocalPlayer*>)
int32_t g_playerCtrlOff    = -1;   // UPlayer::PlayerController

// The GameInstance, cached as a raw pointer and index rather than a CachedObjRef, whose Alive
// consults this module and would recurse.
void*   g_gameInstance     = nullptr;
int32_t g_gameInstanceIdx  = -1;

// The UE4.27 TArray header: data, num, max.
struct ArrayHeader {
    void*   data;
    int32_t num;
    int32_t max;
};

// The class of `obj`, or null; split out so the outer climb reads once.
inline void* ClassOfSafe(void* o) { return o ? R::ClassOf(o) : nullptr; }

// Resolve the three property offsets and two classes, all by name, so a game recook is a loud
// degraded miss rather than a wrong read at a stale offset. The native declaring classes are
// asked, not the game's blueprint subclasses: the first cached-reference stamp of the process
// runs during boot, before any blueprint class has loaded, while the native classes are
// registered at static init. And it retries: a one-shot latch during boot turns a transient
// miss into a permanent one, so it latches only on full success.
void EnsureResolved() {
    if (g_resolved) return;
    // Each attempt costs class walks, and Alive is a hot path, so an unresolved state must not
    // re-walk at the caller's rate.
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
    // Still incomplete: reported once, and only after a grace period, since during boot an
    // incomplete answer is normal. Retries continue slowly after the report, so a late
    // registration still recovers.
    if (now - sFirstAttemptMs >= 30000) {
        LogResolutionStateOnce();
        sNextAttemptMs = now + 30000;
    }
}

// The GameInstance, revalidated by slot, never by dereferencing a possibly freed pointer;
// process-immortal in practice.
void* GameInstance() {
    if (g_gameInstance && !R::IsLiveByIndex(g_gameInstance, g_gameInstanceIdx)) {
        g_gameInstance = nullptr;
        g_gameInstanceIdx = -1;
    }
    if (!g_gameInstance) {
        // One object-array walk, cached for the process; the only walk in this module.
        g_gameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        g_gameInstanceIdx = g_gameInstance ? R::InternalIndexOf(g_gameInstance) : -1;
    }
    return g_gameInstance;
}

// The local PlayerController the engine currently owns: GameInstance, LocalPlayers[0],
// PlayerController. The local player outlives world travel (it is outered to the
// GameInstance), and the engine repoints its controller field at each new world's
// controller, which is exactly the travel signal a liveness test cannot see.
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

// The refresh cadence: 100 ms is two orders below a world transition and two orders above the
// per-frame rate Alive is asked at, so the read is effectively free and the staleness
// invisible.
constexpr unsigned long long kRefreshMs = 100;

void RefreshOnGameThread_() {
    // The re-entrancy brake: this path calls reflection, reflection caches classes through
    // CachedObjRef, and its Alive calls CurrentWorld, which lands here. One nested level is
    // harmless; a loop is not.
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
    // WorldOf dereferences the controller and climbs its outer chain, so the controller is
    // validated first, with the fresh-pointer contract (bare IsLive), legitimate because it was
    // read out of the engine's own field a few instructions ago rather than cached across tasks;
    // otherwise the refresh is an unguarded multi-object deref at 10 Hz through world teardown,
    // surviving only because the engine nulls strong references at GC.
    void* world = (pc && R::IsLive(pc)) ? WorldOf(pc) : nullptr;

    // Classified here, where the world is a pointer the engine handed over microseconds ago, not
    // a cached one: naming a world requires dereferencing it, and every consumer is forbidden to.
    WorldKind kind = WorldKind::Unknown;
    if (world && R::IsLive(world)) {
        kind = R::NameContains(R::NameOf(world), kGameplayWorldSubstr) ? WorldKind::Gameplay
                                                                      : WorldKind::Other;
    }
    g_worldKind.store(kind, std::memory_order_relaxed);

    void* prev = g_currentWorld.exchange(world, std::memory_order_relaxed);
    if (prev != world) {
        g_generation.fetch_add(1, std::memory_order_relaxed);
        // The edge, at info: the line that answers why a cache survived a travel.
        UE_LOGI("world_identity: current world %p -> %p (gen=%u, pc=%p)", prev, world,
                g_generation.load(std::memory_order_relaxed), pc);
    }
}

}  // namespace

void LogResolutionStateOnce() {
    // The flag is refreshed on every call; only the log line is once, or a transient boot-window
    // miss would be reported as permanent after a later retry succeeded. The classes belong in
    // the predicate too: WorldOf rejects unconditionally on either class, so a class-name break
    // would leave every world term dead while a line reported health.
    const bool bad = (g_owningWorldOff < 0 || g_localPlayersOff < 0 || g_playerCtrlOff < 0 ||
                      g_levelCls == nullptr || g_worldCls == nullptr);
    g_degraded.store(bad, std::memory_order_relaxed);
    static bool sLogged = false;
    if (sLogged) return;
    sLogged = true;
    if (bad) {
        // A permanent negative latch with no diagnostic is how a recook silently brings the
        // stale-pawn window back; every term is named, so the log says which one moved.
        UE_LOGE("world_identity: DEGRADED -- ULevel::OwningWorld=%d "
                "UGameInstance::LocalPlayers=%d UPlayer::PlayerController=%d "
                "(-1 = not found; ULevel=%p UWorld=%p, null = the CLASS itself did not "
                "resolve). Every world-currency term in the tree now fails "
                "OPEN, i.e. back to liveness-only caches and the "
                "stale-cross-world-pawn class of bug. A game recook that renamed one "
                "of these fields looks exactly like this.",
                g_owningWorldOff, g_localPlayersOff, g_playerCtrlOff, g_levelCls, g_worldCls);
    } else {
        UE_LOGI("world_identity: resolved OwningWorld=+0x%X LocalPlayers=+0x%X "
                "PlayerController=+0x%X", g_owningWorldOff, g_localPlayersOff,
                g_playerCtrlOff);
    }
}

// A pure read that deliberately does not call EnsureResolved, and that is load-bearing:
// CachedObjRef's Set calls this, and reflection calls Set from PrimeClassWalk while holding
// its class-cache mutex, a non-recursive one, so nothing reachable from here may take it,
// and EnsureResolved can, through GameInstance and FindObjectByClass. Resolution belongs to
// the refresh path, entered from Alive rather than Set. Before resolution this answers null,
// no world term, for the handful of objects stamped during the boot window.
void* WorldOf(void* obj) {
    if (!obj) return nullptr;
    if (!g_levelCls || !g_worldCls || g_owningWorldOff < 0) return nullptr;
    // A bounded climb: an actor is outered to its level (one step), a component to its actor
    // (two), a nested subobject a little deeper. 8 is past anything the engine builds and makes
    // a corrupted outer ring terminate.
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
    // Not world-scoped: a class, a function, a CDO, a cooked asset, the GameInstance; their outer
    // chain reaches a package and stops. Null here means no world term, not a failed lookup (see
    // Degraded).
    return nullptr;
}

void* CurrentWorld() {
    // Only the game thread may walk engine memory; everyone else reads the publish.
    if (ue_wrap::game_thread::IsGameThread()) RefreshOnGameThread_();
    return g_currentWorld.load(std::memory_order_relaxed);
}

uint32_t Generation() { return g_generation.load(std::memory_order_relaxed); }

WorldKind CurrentWorldKind() {
    // The same shape as CurrentWorld: the game thread drives the refresh, everyone else reads the
    // publish, so a consumer that only asks for the kind still keeps the memo warm.
    if (ue_wrap::game_thread::IsGameThread()) RefreshOnGameThread_();
    return g_worldKind.load(std::memory_order_relaxed);
}

bool Degraded() { return g_degraded.load(std::memory_order_relaxed); }

// The dev instrument. The design that consumes this module rests on one measured premise:
// that the local player's controller field moves at a solo quit-to-menu. Pass means that
// during a menu window after solo play this module's world differs from the world of the
// pawn the registry still hands out.
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

    // Candidate B's ambiguity, measured: how many live worlds exist right now. If a dying world
    // lingers in the array, FindObjectByClass answers whichever is indexed first, which is why B
    // is not the owner.
    int liveWorlds = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || R::ClassOf(o) != g_worldCls) continue;
        if (!R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        ++liveWorlds;
    }
    // The verdict field: the world of whatever the registry still believes is the local pawn.
    // Stale is the storm condition, a pawn from a world no longer current, which a liveness-only
    // cache cannot see.
    void* pawnWorld = localPawnForCompare ? WorldOf(localPawnForCompare) : nullptr;
    const int stale = (localPawnForCompare && pawnWorld && wA && pawnWorld != wA) ? 1 : 0;

    // The drill. The fields above read WorldOf live off the pawn, which is not the shipped
    // predicate: CachedObjRef stamps at Set and never reads the object again, and the storm does
    // not reproduce on every machine (the dead pawn may purge within seconds), so the shipped
    // path could ship having never been observed to reject anything. So the pawn is latched
    // through the real CachedObjRef once, while healthy, and the shipped verdict is reported
    // beside the raw slot liveness: alive false with the slot live is the fix working.
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

    // The negative control: everything above can only show the predicate accepting, since when
    // the dead pawn's slot dies within a second the liveness term rejects first and the world
    // term never decides. So force it: hold the object, the slot and the serial constant, change
    // only the current world, and require the verdict to flip. If this prints pass with the
    // latch still alive, the world term is not participating in Alive at all.
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
            // A sentinel that is not any world and is never dereferenced; the module treats these
            // as comparison tokens.
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
