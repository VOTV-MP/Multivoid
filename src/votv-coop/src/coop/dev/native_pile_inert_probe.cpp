// coop/dev/native_pile_inert_probe.cpp -- see coop/dev/native_pile_inert_probe.h.

#include "coop/dev/native_pile_inert_probe.h"

#include "coop/config/config.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::native_pile_inert_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using clock = std::chrono::steady_clock;

enum class St { Idle, Settle, Sampling, Done };
St      g_st    = St::Idle;
clock::time_point g_t0{};      // settle start (Settle) / spawn time (Sampling)
void*   g_actor = nullptr;
int32_t g_idx   = -1;
int     g_lastSec = -1;

constexpr int kSettleSec  = 8;
constexpr int kWatchSec   = 60;

// The local player pawn (mainPlayer_C with a controller -- the definitive local-vs-puppet
// discriminator). One GUObjectArray walk at the settle edge, NOT per tick. nullptr until the world
// + player exist.
void* LocalPlayer() {
    for (void* p : R::FindObjectsByClass(L"mainPlayer_C")) {
        if (p && R::IsLive(p) && E::GetController(p)) return p;
    }
    return nullptr;
}

void Cleanup() {
    if (g_actor) {
        R::RemoveFromRoot(g_actor);   // un-pin BEFORE destroy (rooted PendingKill would leak)
        E::DestroyActor(g_actor);
        g_actor = nullptr;
    }
}

}  // namespace

void Install() { /* no boot work -- the probe drives entirely from Tick's state machine */ }

void Tick(bool connected, bool isHost) {
    static const bool enabled = coop::config::IsIniKeyTrue("native_pile_inert_probe");
    if (!enabled) return;
    UE_ASSERT_GAME_THREAD("native_pile_inert_probe::Tick");

    const auto now = clock::now();
    switch (g_st) {
    case St::Idle:
        g_t0 = now;
        g_st = St::Settle;
        UE_LOGI("[INERT-PROBE] armed (ini native_pile_inert_probe=1) -- settling %ds before the runtime "
                "spawn (connected=%d host=%d). Best run as HOST or standalone New Game.",
                kSettleSec, connected, isHost);
        break;

    case St::Settle: {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - g_t0).count() < kSettleSec) break;
        void* player = LocalPlayer();
        if (!player) break;  // world/player not up yet -- keep waiting
        void* cls = R::FindClass(L"actorChipPile_C");
        if (!cls) {
            UE_LOGW("[INERT-PROBE] actorChipPile_C class not loaded -- cannot spawn; abort.");
            g_st = St::Done;
            break;
        }
        // Spawn ~2.5 m in FRONT of the player on their ground plane so the COLLISION-ON native both
        // CONTACTS the world (excites the contact/overlap path the static RE only ASSUMED is inert --
        // the pile has no contact-convert gate, but that "should hold" is unverified on the SAME axis as
        // the original confound) AND is reachable for the hands-on preview (aim -> native hover GUI?
        // walk into -> movement-block?).
        const ue_wrap::FVector pl = E::GetActorLocation(player);
        const ue_wrap::FRotator cr = E::GetCameraRotation();
        const float yaw = cr.Yaw * 3.14159265f / 180.f;
        const ue_wrap::FVector loc{ pl.X + std::cos(yaw) * 250.f, pl.Y + std::sin(yaw) * 250.f, pl.Z };
        void* a = E::SpawnActor(cls, loc, /*inertPawn=*/false);
        if (!a) {
            UE_LOGW("[INERT-PROBE] SpawnActor(actorChipPile_C) FAILED -- abort.");
            g_st = St::Done;
            break;
        }
        // The ACTUAL nativization recipe -- COLLISION-ON (the native's own profile = the trace surface +
        // movement-block), Movable (so the host can position it / b3 can correct it), tick-off (no
        // autonomous ubergraph), AddToRoot (GC-pin). NoCollision was already proven inert 39s; THIS run
        // excites the collision-ON contact path. Collision is left at the native default -- deliberately
        // NOT disabled.
        R::AddToRoot(a);                              // GC-pin (the proxy's proven prophylactic)
        E::SetActorTickEnabled(a, false);             // no per-frame ubergraph
        if (void* comp = E::GetStaticMeshComponent(a)) E::SetComponentMobility(comp, 2);  // Movable (host-positionable)
        g_actor   = a;
        g_idx     = R::InternalIndexOf(a);
        g_t0      = now;
        g_lastSec = -1;
        g_st      = St::Sampling;
        UE_LOGI("[INERT-PROBE] SPAWNED rooted runtime actorChipPile_C actor=%p idx=%d at (%.0f,%.0f,%.0f) "
                "-- COLLISION-ON recipe (native collision + AddToRoot + tick-off + Movable), ~2.5m ahead. "
                "Watching %ds for self-destruct/self-morph from the contact path. ALSO: aim at it (native "
                "hover GUI?) + walk into it (movement-block?).",
                a, g_idx, loc.X, loc.Y, loc.Z, kWatchSec);
        break;
    }

    case St::Sampling: {
        const int sec = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - g_t0).count());
        if (sec == g_lastSec) break;  // one sample/second
        g_lastSec = sec;

        if (!R::IsLiveByIndex(g_actor, g_idx)) {
            UE_LOGW("[INERT-PROBE] VERDICT NO-GO @t=%ds: the actor went NOT-LIVE despite AddToRoot -- a "
                    "rooted runtime chipPile SELF-DESTRUCTS via its own ubergraph (autonomy, NOT GC). "
                    "The bare proxy is load-bearing; do NOT nativize. (Nothing to clean up -- already gone.)",
                    sec);
            g_actor = nullptr;
            g_st = St::Done;
            break;
        }
        const std::wstring cls = R::ClassNameOf(g_actor);
        if (cls != L"actorChipPile_C") {
            UE_LOGW("[INERT-PROBE] VERDICT NO-GO @t=%ds: the actor SELF-MORPHED to '%ls' on its own "
                    "(ubergraph autonomy). The bare proxy is load-bearing; do NOT nativize.",
                    sec, cls.c_str());
            Cleanup();
            g_st = St::Done;
            break;
        }
        UE_LOGI("[INERT-PROBE] t=%ds live=1 class=actorChipPile_C", sec);
        if (sec >= kWatchSec) {
            UE_LOGI("[INERT-PROBE] VERDICT GO: a rooted runtime actorChipPile_C stayed INERT (live + pile) "
                    "for %ds -- the BP's ubergraph does NOT self-destruct/self-morph when left alone. "
                    "NATIVIZE resting/runtime/re-pile (rooted real native = native hover GUI + collision "
                    "+ rotation free); retire the bare proxy + the rotation-fix + the hover-GUI under RULE 2.",
                    kWatchSec);
            Cleanup();
            g_st = St::Done;
        }
        break;
    }

    case St::Done:
        break;
    }
}

}  // namespace coop::dev::native_pile_inert_probe
