// coop/flashlight_click_sound.cpp -- see flashlight_click_sound.h.
//
// Extracted from item_activate.cpp on 2026-05-26 (per modular soft-cap
// rule: item_activate.cpp had grown to 853 LOC > 800 soft cap, and the
// click-sound subsystem is conceptually distinct from the flashlight
// state-sync wire / observer logic).

#include "coop/player/flashlight_click_sound.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <array>

namespace coop::flashlight_click_sound {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Per-peer last-applied state. Keyed on peerSlot NOT raw puppet pointer
// (the pointer would dangle after disconnect + respawn for the same peer
// slot). Sized to coop::players::kMaxPeers so a future bump of the central
// constant propagates here automatically (the prior local `kMaxPeers = 4`
// hardcode silently capped at 4 regardless). -1 = no apply yet (first
// packet's state always differs -> click plays).
//
// ApplyToPuppet (the only caller) runs on the game thread via GT::Post
// so plain int is safe -- no atomic needed.
std::array<int, coop::players::kMaxPeers> g_lastAppliedStateByPeer = []{
    std::array<int, coop::players::kMaxPeers> a{};
    a.fill(-1);
    return a;
}();

}  // namespace

void PlayIfStateChanged(void* puppetActor, uint8_t peerSlot, bool newState) {
    if (!puppetActor) return;

    // 1) State-change gate: skip if state matches last apply for this peer.
    //    Hold-F mode-change packets keep state=on (just mutate cones) -- those
    //    MUST NOT click. Press-F toggles always pass.
    const int curState = newState ? 1 : 0;
    bool stateChanged = false;
    if (peerSlot < coop::players::kMaxPeers) {
        stateChanged = (g_lastAppliedStateByPeer[peerSlot] != curState);
        g_lastAppliedStateByPeer[peerSlot] = curState;
    } else {
        // Out-of-range peer slot (defensive). Treat every apply as a state
        // change so we still click; the array is sized to the central
        // coop::players::kMaxPeers so this branch only fires on a
        // legitimately invalid peerSlot.
        stateChanged = true;
    }
    if (!stateChanged) return;

    // 2) Lazy-resolve the sound asset + attenuation. Each retried until
    //    non-null so we don't permanently cache nullptr if an early packet
    //    arrives before asset load. The GameplayStatics CDO + the
    //    PlaySoundAtLocation UFunction are resolved + cached INSIDE
    //    ue_wrap::engine::PlaySoundAtLocation (section 5) now, so they no
    //    longer live here (RULE 2 -- one dispatch, shared with the trash-clump
    //    throw whoosh in coop::prop_sound).
    static void* sSoundAsset  = nullptr;
    static void* sAttenuation = nullptr;

    if (!sSoundAsset) {
        sSoundAsset = R::FindObject(P::name::FlashlightClickSoundName,
                                    P::name::SoundWaveClass);
    }

    // 3) RULE 1 native path: construct our own USoundAttenuation via
    //    UGameplayStatics::SpawnObject. No coupling to VOTV's cooked
    //    `att_*` content assets. A-3 (2026-05-29) Principle 7: the
    //    SpawnObject UFunction call + the 8 raw att:: offset writes +
    //    AddToRoot now live behind ue_wrap::engine::SpawnSoundAttenuation;
    //    gameplay code holds only the cached pointer.
    //
    //    Sphere shape. History: 2m/20m (initial) was too short -> bumped
    //    10x to 20m full-volume / 200m falloff (2026-05-26) so the click
    //    carried across VOTV's outdoor map. User feedback 2026-06-03: that
    //    is too FAR for a REMOTE player's toggle ("радиус звука включения
    //    фонарика у puppet понизить в 3 раза") -- reduce the radius 3x. We
    //    scale BOTH the full-volume sphere radius AND the falloff distance
    //    by 1/3 so the whole envelope shrinks uniformly (6.67m full-volume
    //    -> ~73m silence, was 20m -> 220m = exactly 1/3 the reach).
    //
    //    Puppet-only by construction: PlayIfStateChanged is ONLY ever
    //    called from item_activate::ApplyToPuppet (the receiver path); the
    //    local player's own flashlight click is VOTV's native sound, which
    //    this code never touches. So this 1/3 only affects how far OTHERS
    //    hear a remote player's flashlight toggle.
    if (!sAttenuation) {
        ue_wrap::engine::SoundAttenuationConfig cfg{};  // generic baseline
        cfg.extents[0]      /= 3.f;   // 2000cm (20m)   -> 667cm  (6.67m)
        cfg.falloffDistance /= 3.f;   // 20000cm (200m) -> 6667cm (66.7m)
        sAttenuation = ue_wrap::engine::SpawnSoundAttenuation(cfg);
        if (sAttenuation) {
            UE_LOGI("flashlight: constructed native USoundAttenuation %p "
                    "(sphere r=6.67m, falloff=66.7m, inverse; radius/3 per "
                    "2026-06-03 puppet-volume req)", sAttenuation);
        }
    }

    // 4) Read puppet world location via the existing GetActorLocation wrapper.
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(puppetActor);

    // 5) Fire the click via the shared PlaySoundAtLocation wrapper (resolves +
    //    caches the GameplayStatics CDO + UFunction internally; tolerates a
    //    null attenuation -> 2D). Owned by the puppet so it plays in its world.
    if (sSoundAsset) {
        ue_wrap::engine::PlaySoundAtLocation(puppetActor, sSoundAsset, loc, sAttenuation);
        UE_LOGI("flashlight: click sound played at puppet pos (%.0f, %.0f, %.0f) "
                "attenuation=%p (sphere r=6.67m falloff=66.7m)",
                loc.X, loc.Y, loc.Z, sAttenuation);
    } else {
        UE_LOGW("flashlight: click sound NOT played -- asset unresolved (%p)", sSoundAsset);
    }
}

}  // namespace coop::flashlight_click_sound
