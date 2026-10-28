// coop/props/trash_native_hold.cpp -- see coop/props/trash_native_hold.h.

#include "coop/props/trash_native_hold.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/pile_late_load.h"          // [dev] the late-load seam keeps one held clump aside
#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor (a bound or minted actor is not a bare load spawn)
#include "coop/session/world_load_episode.h"  // InEpisode: the join's world load
#include "ue_wrap/actors/prop.h"              // IsGarbageClump
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace coop::trash_native_hold {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

UE_FACT_FN(kRowFinishSpawningActor, "trash_native_hold", L"GameplayStatics", L"FinishSpawningActor", Declared);

// The session the catch reads. The hook carries no session of its own.
std::atomic<coop::net::Session*> g_session{nullptr};

bool     g_hookInstalled = false;   // game thread only
bool     g_gaveUp        = false;
int      g_tries         = 0;
uint64_t g_nextTryMs     = 0;
constexpr uint64_t kResolveRetryMs  = 1000;
constexpr int      kMaxResolveTries = 30;

uint32_t g_heldCount = 0;           // game thread only; per process
uint32_t g_retiredCount = 0;

// The bodies the catch held, by pointer, each with the slot's serial, so a recycled address is not a
// held body. Session-scoped: ReleaseUnbound and OnDisconnect clear it.
std::unordered_map<void*, ue_wrap::CachedObjRef> g_held;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The catch: a post-hook on FinishSpawningActor, which every actor spawn in the process passes, so
// the gates run cheapest first and the host and a solo game leave at the first.
//
// Why a clump held here lands before it moves, and a pile needs no hold: the save load spawns each
// object deferred and finishes it at its saved transform (the gamemode's objectsData loop), so at
// this POST the clump has its saved pose and has run BeginPlay, and no physics step runs inside
// FinishSpawningActor, so the hold lands before its first move. The collider reports its overlaps
// inside the Finish, before this POST; a pile's overlap handler calls `toClump`, which
// trash_morph_gate refuses on a connected client (a name watch resolved from the session's first
// ticks), so nothing has morphed by now. A mirror the host's rows spawn arrives after the load tail
// quiesced, when the episode is over, so this window holds only the save's own bodies.
void OnFinishSpawn(void* /*context*/, void* /*srcObj*/, void* result) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client || !s->connected()) return;
    if (!coop::world_load_episode::InEpisode()) return;
    if (!result || !GT::IsGameThread()) return;
    if (!ue_wrap::prop::IsGarbageClump(result)) return;
    if (!R::IsLive(result)) return;
    // A drill red (ini trash_hold_red=1): hold nothing, so pile_defer_drill=check must fail on orphans.
    static const bool kRed = coop::config::ResolveInt(coop::config_registry::rows::trash_hold_red) != 0;
    if (kRed) return;
    if (PT::GetPropElementIdForActor(result) != coop::element::kInvalidId) return;
    Hold(result);
    g_held[result].Set(result);
    coop::dev::pile_late_load::NoteLoadCaught(result);   // [dev] the late-load seam (a cached bool read when off)
    if (++g_heldCount == 1 || g_heldCount % 100 == 0)
        UE_LOGI("trash_native_hold: held save-loaded clump %p at its load spawn (#%u) -- tick, physics off, "
                "until the bind or the divergence sweep", result, g_heldCount);
}

}  // namespace

// Authority shape. MTA lets only an element's syncer simulate it: the server picks one client per
// unoccupied vehicle (reference/mtasa-blue/Server/mods/deathmatch/logic/
// CUnoccupiedVehicleSync.cpp:171-203), and a client that is not the syncer does not simulate it but
// drives it to the syncer's reports (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CClientVehicle.cpp:3715-3733). Source's client physics prop has no physics object at all
// (reference/source-sdk-2013/src/game/client/c_physicsprop.h:17-33).
// Here the host syncs an unbound native until the joiner binds it, so the joiner holds its body
// still: the pose it was loaded at is its identity key, and any local motion before the bind loses it.
void Hold(void* actor) {
    ue_wrap::engine::SetActorTickEnabled(actor, false);      // a clump's tick wakes its body every second
    ue_wrap::engine::SetActorSimulatePhysics(actor, false);  // kinematic: the host drives it
    ue_wrap::engine::SetActorRootMovable(actor);             // else SetActorLocation silently no-ops on a Static root
}

void Release(void* actor) {
    ue_wrap::engine::SetActorTickEnabled(actor, true);
    ue_wrap::engine::SetActorSimulatePhysics(actor, true);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_hookInstalled || g_gaveUp) return;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kResolveRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinishSpawningActor);
    if (fn && ue_wrap::ufunction_hook::InstallPostHook(fn, &OnFinishSpawn)) {
        g_hookInstalled = true;
        UE_LOGI("trash_native_hold: load catch installed on FinishSpawningActor");
    } else if (++g_tries >= kMaxResolveTries) {
        g_gaveUp = true;
        UE_LOGE("trash_native_hold: FinishSpawningActor did not resolve or would not take a post hook in %d "
                "tries; a joiner's save-loaded clumps are not held", kMaxResolveTries);
    }
}

bool IsHeld(void* actor) {
    if (!actor || g_held.empty()) return false;
    const auto it = g_held.find(actor);
    return it != g_held.end() && it->second.Is(actor);
}

void NoteRetired(void* actor) {
    ++g_retiredCount;
    if (g_retiredCount == 1 || g_retiredCount % 100 == 0)
        UE_LOGI("trash_native_hold: destroyed held clump %p -- the host never bound it, so it is not in the "
                "host's world (#%u of %u held)", actor, g_retiredCount, g_heldCount);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_held.clear();
}

void ReleaseUnbound() {
    // With no session nobody authors a held clump the bind never took: left kinematic with its tick off
    // it would be a stuck body in the single-player world, and the game's pickup wants a simulating
    // one. A bound clump is pile_spawn_bind's parked one and goes back there; a destroyed one is dead
    // by serial.
    int restored = 0;
    for (const auto& [actor, ref] : g_held) {
        if (!ref.Is(actor) || PT::IsBoundMirrorNative(actor)) continue;
        Release(actor);
        ++restored;
    }
    if (!g_held.empty())
        UE_LOGI("trash_native_hold: ReleaseUnbound gave %d of %zu held clump(s) back to the game (tick and "
                "physics on)", restored, g_held.size());
    g_held.clear();
}

}  // namespace coop::trash_native_hold
