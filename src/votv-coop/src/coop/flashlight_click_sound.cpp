// coop/flashlight_click_sound.cpp -- see flashlight_click_sound.h.
//
// Extracted from item_activate.cpp on 2026-05-26 (per modular soft-cap
// rule: item_activate.cpp had grown to 853 LOC > 800 soft cap, and the
// click-sound subsystem is conceptually distinct from the flashlight
// state-sync wire / observer logic).

#include "coop/flashlight_click_sound.h"

#include "coop/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

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

    // 2) Lazy-resolve UFunction + asset + attenuation pointers. Each
    //    retried until non-null so we don't permanently cache nullptr if
    //    an early packet arrives before asset load.
    //
    //    sGameplayStatics here is the CDO used by section 5
    //    (PlaySoundAtLocation Call target). Post A-3 (2026-05-29) the
    //    SpawnSoundAttenuation wrapper resolves its OWN GameplayStatics
    //    CDO internally (see ResolveAttSpawn in engine.cpp), so the
    //    !sAttenuation block no longer depends on this static being
    //    populated first. Order kept stable for readability.
    static void* sGameplayStatics = nullptr;
    static void* sPlaySoundFn     = nullptr;
    static void* sSoundAsset      = nullptr;
    static void* sAttenuation     = nullptr;

    if (!sGameplayStatics) {
        sGameplayStatics = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    }
    if (!sPlaySoundFn && sGameplayStatics) {
        if (void* gsCls = R::ClassOf(sGameplayStatics)) {
            sPlaySoundFn = R::FindFunction(gsCls, P::name::PlaySoundAtLocationFn);
        }
    }
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
    //    Sphere shape, 20m audible radius, 200m falloff. User feedback
    //    2026-05-26: 2m/20m was too short -- bumped 10x so the click is
    //    audible across a meaningful traversal distance on VOTV's
    //    outdoor map. Defaults of SoundAttenuationConfig match exactly.
    if (!sAttenuation) {
        ue_wrap::engine::SoundAttenuationConfig cfg{};  // VOTV flashlight defaults
        sAttenuation = ue_wrap::engine::SpawnSoundAttenuation(cfg);
        if (sAttenuation) {
            UE_LOGI("flashlight: constructed native USoundAttenuation %p "
                    "(sphere r=20m, falloff=200m, inverse)", sAttenuation);
        }
    }

    // 4) Read puppet world location via the existing GetActorLocation
    //    wrapper (replaces the prior local ActorClass + UFunction cache
    //    duplicate). Rotation stays at zero (PlaySoundAtLocation does
    //    not use orientation for a non-cone spatialised source).
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(puppetActor);
    const ue_wrap::FRotator rot{};

    // 5) Fire PlaySoundAtLocation. Tolerates nullptr attenuation -- plays
    //    2D in that case (e.g. SpawnObject failed at startup). Per-call
    //    transient UAudioComponent is engine-managed (auto-destroy when
    //    playback finishes).
    if (sSoundAsset && sGameplayStatics && sPlaySoundFn) {
        ue_wrap::ParamFrame f(sPlaySoundFn);
        f.Set<void*>(L"WorldContextObject", puppetActor);
        f.Set<void*>(L"Sound", sSoundAsset);
        f.SetRaw(L"Location", &loc, sizeof(loc));
        f.SetRaw(L"Rotation", &rot, sizeof(rot));
        f.Set<float>(L"VolumeMultiplier", 1.f);
        f.Set<float>(L"PitchMultiplier", 1.f);
        f.Set<float>(L"StartTime", 0.f);
        f.Set<void*>(L"AttenuationSettings", sAttenuation);
        f.Set<void*>(L"ConcurrencySettings", nullptr);
        f.Set<void*>(L"OwningActor", puppetActor);
        ue_wrap::Call(sGameplayStatics, f);
        UE_LOGI("flashlight: click sound played at puppet pos (%.0f, %.0f, %.0f) "
                "attenuation=%p (sphere r=20m falloff=200m)",
                loc.X, loc.Y, loc.Z, sAttenuation);
    } else {
        UE_LOGW("flashlight: click sound NOT played -- asset=%p gs=%p fn=%p",
                sSoundAsset, sGameplayStatics, sPlaySoundFn);
    }
}

}  // namespace coop::flashlight_click_sound
