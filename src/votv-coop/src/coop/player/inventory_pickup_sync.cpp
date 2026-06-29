// coop/inventory_pickup_sync.cpp -- see coop/inventory_pickup_sync.h.

#include "coop/player/inventory_pickup_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::inventory_pickup_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolution / registration latches (Install retries throttled until done).
void*   g_playSound2DFn = nullptr;
void*   g_inventoryCue  = nullptr;  // the cue OBJECT -- the observer predicate is a pointer compare
int32_t g_offSound = -1;
int32_t g_offPitch = -1;
int32_t g_offWco   = -1;
bool    g_observerRegistered = false;
uint32_t g_resolveN = 0;  // ~1 Hz throttle on the resolve walk (Install runs per pump tick)

// POST observer on UGameplayStatics::PlaySound2D. Dispatches at human-event
// rate game-wide (UI clicks, 2D cues); the body is three cached-offset reads
// + compares, exiting on the first mismatch. The predicate (RE 2026-06-11,
// votv-inventory-pickup-seam-RE: complete game-wide cue census):
//   Sound == inventory_Cue   -- pointer compare vs the resolved cue object
//   pitch in (1.05, 1.2)     -- the collect plays 1.1; dream-wake 0.9 and
//                               customWall 2.0 rejected
//   WorldContextObject == the LOCAL player -- the collector (a puppet has no
//                               input stack / tick; it can never dispatch this)
// -> fires exactly once per successful inventory collect.
void OnPlaySound2DPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread() || !params) return;
    if (!g_inventoryCue) return;  // cue not resolved yet -> predicate undecidable
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    const auto* p = static_cast<const uint8_t*>(params);
    void* sound = *reinterpret_cast<void* const*>(p + g_offSound);
    if (sound != g_inventoryCue) return;
    const float pitch = *reinterpret_cast<const float*>(p + g_offPitch);
    if (pitch <= 1.05f || pitch >= 1.2f) return;  // dream-wake / customWall reject
    void* wco = *reinterpret_cast<void* const*>(p + g_offWco);
    void* local = coop::players::Registry::Get().Local();
    if (!local || wco != local) return;

    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) return;
    coop::net::InventoryPickupPayload payload{loc.X, loc.Y, loc.Z};
    s->SendReliable(coop::net::ReliableKind::InventoryPickup, &payload, sizeof(payload));
    UE_LOGI("inventory_pickup: broadcast collect blip at (%.0f, %.0f, %.0f)",
            loc.X, loc.Y, loc.Z);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_observerRegistered && g_inventoryCue) return;

    // Throttle the GUObjectArray walks to ~1 Hz until everything resolves
    // (the cue asset streams in with gameplay; GameplayStatics exists at boot).
    if ((g_resolveN++ % 125) != 0) return;

    if (!g_inventoryCue) {
        g_inventoryCue = R::FindObject(L"inventory_Cue", L"SoundCue");
        if (g_inventoryCue)
            UE_LOGI("inventory_pickup: resolved inventory_Cue=%p", g_inventoryCue);
    }
    if (g_observerRegistered) return;

    if (!g_playSound2DFn) {
        if (void* cls = R::FindClass(L"GameplayStatics"))
            g_playSound2DFn = R::FindFunction(cls, L"PlaySound2D");
        if (!g_playSound2DFn) return;  // engine class not indexed yet -- retry
        g_offSound = R::FindParamOffset(g_playSound2DFn, L"Sound");
        g_offPitch = R::FindParamOffset(g_playSound2DFn, L"PitchMultiplier");
        g_offWco   = R::FindParamOffset(g_playSound2DFn, L"WorldContextObject");
        if (g_offSound < 0 || g_offPitch < 0 || g_offWco < 0) {
            UE_LOGW("inventory_pickup: PlaySound2D param offsets unresolved "
                    "(Sound=%d Pitch=%d WCO=%d) -- blip sync disabled",
                    g_offSound, g_offPitch, g_offWco);
            // Permanently DISABLED (no retry): a recook changed the signature;
            // re-walking it would yield the same miss. The observer is never
            // registered in this state, so the -1 offsets are unreachable.
            g_playSound2DFn = nullptr;
            g_observerRegistered = true;
            return;
        }
    }
    if (!GT::RegisterPostObserver(g_playSound2DFn, &OnPlaySound2DPost)) {
        UE_LOGE("inventory_pickup: POST observer registration FAILED (table full?) -- retrying");
        return;
    }
    g_observerRegistered = true;
    UE_LOGI("inventory_pickup: observer installed on GameplayStatics::PlaySound2D @ %p "
            "(offs Sound=%d Pitch=%d WCO=%d, cue=%p)",
            g_playSound2DFn, g_offSound, g_offPitch, g_offWco, g_inventoryCue);
}

void OnReliable(const coop::net::InventoryPickupPayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("inventory_pickup: OnReliable off-game-thread -- dropping"); return; }
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z)) return;
    void* worldCtx = coop::players::Registry::Get().Local();
    if (!worldCtx) return;  // no local pawn yet -> no world to play in
    coop::prop_sound::PlayInventoryBlipAt(worldCtx, ue_wrap::FVector{payload.x, payload.y, payload.z});
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // The observer stays registered; it self-gates on a connected session.
}

}  // namespace coop::inventory_pickup_sync
