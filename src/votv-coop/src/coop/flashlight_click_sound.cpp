// coop/flashlight_click_sound.cpp -- see flashlight_click_sound.h.
//
// Extracted from item_activate.cpp on 2026-05-26 (per modular soft-cap
// rule: item_activate.cpp had grown to 853 LOC > 800 soft cap, and the
// click-sound subsystem is conceptually distinct from the flashlight
// state-sync wire / observer logic).

#include "coop/flashlight_click_sound.h"

#include "coop/players_registry.h"
#include "ue_wrap/call.h"
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
    //    Resolution order MATTERS: sGameplayStatics MUST be resolved
    //    BEFORE the !sAttenuation block, because that block needs the
    //    CDO to look up SpawnObject + to use as Outer for SpawnObject.
    //    Earlier ordering bug (audit 2026-05-26 v2): sGameplayStatics
    //    was populated AFTER the attenuation block, so the first
    //    state-change packet of each session left sAttenuation null
    //    and the first remote click played 2D until the next packet.
    static void* sGameplayStatics = nullptr;
    static void* sPlaySoundFn     = nullptr;
    static void* sSoundAsset      = nullptr;
    static void* sAttenuation     = nullptr;
    static void* sGetLocFn        = nullptr;

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
    //    `att_*` content assets. Configure radii + spatial flags
    //    directly via reflected field offsets (sdk_profile `att::*`).
    if (!sAttenuation) {
        static void* sSpawnObjectFn = nullptr;
        if (!sSpawnObjectFn && sGameplayStatics) {
            if (void* gsCls = R::ClassOf(sGameplayStatics)) {
                sSpawnObjectFn = R::FindFunction(gsCls, P::name::SpawnObjectFn);
            }
        }
        void* attCls = R::FindClass(P::name::SoundAttenuationClass);
        if (sSpawnObjectFn && attCls && sGameplayStatics) {
            ue_wrap::ParamFrame sf(sSpawnObjectFn);
            // CASE-SENSITIVE param names per UE reflection: existing
            // project pattern in engine_widget.cpp:95-96 uses
            // `objectClass` (lowercase 'o') + `Outer`. An uppercase-O
            // ObjectClass would silently SetRaw-fail (FName mismatch)
            // and SpawnObject would receive a null class -> return null
            // -> 2D sound (which is exactly what we hit on first deploy
            // 2026-05-26).
            sf.Set<void*>(L"objectClass", attCls);
            sf.Set<void*>(L"Outer", sGameplayStatics);
            if (ue_wrap::Call(sGameplayStatics, sf)) {
                sAttenuation = sf.Get<void*>(L"ReturnValue");
            }
            if (sAttenuation) {
                // CRITICAL: AddToRoot so UE4 GC never collects this
                // object. We hold the pointer in a C++ static which is
                // INVISIBLE to UE's reachability scan -- without rooting
                // the object gets reaped on the next GC pass and the
                // static dangles. PlaySoundAtLocation would then crash
                // reading freed memory on the next click (root cause of
                // the F-spam crash 2026-05-26).
                R::AddToRoot(sAttenuation);

                auto* p = reinterpret_cast<uint8_t*>(sAttenuation);
                // Sphere shape, 20m audible radius, 200m falloff. User
                // feedback 2026-05-26: 2m/20m was too short -- bumped 10x
                // so the click is audible across a meaningful traversal
                // distance on VOTV's outdoor map.
                *reinterpret_cast<uint8_t*>(p + P::off::att::AttenuationShape)  = 0;   // Sphere
                *reinterpret_cast<uint8_t*>(p + P::off::att::DistanceAlgorithm) = 2;   // Inverse
                *reinterpret_cast<uint8_t*>(p + P::off::att::FalloffMode)       = 0;   // Continues
                float* extents = reinterpret_cast<float*>(p + P::off::att::AttenuationShapeExtents);
                extents[0] = 2000.f;   // sphere radius (cm) -- 20m full-volume zone
                extents[1] = 0.f;
                extents[2] = 0.f;
                *reinterpret_cast<float*>(p + P::off::att::FalloffDistance)    = 20000.f;  // 200m falloff
                *reinterpret_cast<float*>(p + P::off::att::ConeOffset)         = 0.f;
                *reinterpret_cast<float*>(p + P::off::att::dBAttenuationAtMax) = -60.f;
                // Set bAttenuate + bSpatialize bits (everything else is
                // a default initialized by the SoundAttenuation CDO copy
                // when SpawnObject runs).
                uint8_t& flags = *reinterpret_cast<uint8_t*>(p + P::off::att::FlagsByte);
                flags |= 0x01;  // bAttenuate
                flags |= 0x02;  // bSpatialize
                UE_LOGI("flashlight: constructed native USoundAttenuation %p "
                        "(sphere r=20m, falloff=200m, inverse)",
                        sAttenuation);
            }
        }
    }

    // 4) Read puppet world location via K2_GetActorLocation reflection.
    static void* sActorCls = nullptr;
    if (!sActorCls) {
        sActorCls = R::FindClass(P::name::ActorClass);
        if (sActorCls) sGetLocFn = R::FindFunction(sActorCls, P::name::GetActorLocationFn);
    }
    float loc[3] = {0.f, 0.f, 0.f};
    float rot[3] = {0.f, 0.f, 0.f};
    if (sGetLocFn) {
        ue_wrap::ParamFrame f(sGetLocFn);
        ue_wrap::Call(puppetActor, f);
        f.GetRaw(L"ReturnValue", loc, sizeof(loc));
    }

    // 5) Fire PlaySoundAtLocation. Tolerates nullptr attenuation -- plays
    //    2D in that case (e.g. SpawnObject failed at startup). Per-call
    //    transient UAudioComponent is engine-managed (auto-destroy when
    //    playback finishes).
    if (sSoundAsset && sGameplayStatics && sPlaySoundFn) {
        ue_wrap::ParamFrame f(sPlaySoundFn);
        f.Set<void*>(L"WorldContextObject", puppetActor);
        f.Set<void*>(L"Sound", sSoundAsset);
        f.SetRaw(L"Location", loc, sizeof(loc));
        f.SetRaw(L"Rotation", rot, sizeof(rot));
        f.Set<float>(L"VolumeMultiplier", 1.f);
        f.Set<float>(L"PitchMultiplier", 1.f);
        f.Set<float>(L"StartTime", 0.f);
        f.Set<void*>(L"AttenuationSettings", sAttenuation);
        f.Set<void*>(L"ConcurrencySettings", nullptr);
        f.Set<void*>(L"OwningActor", puppetActor);
        ue_wrap::Call(sGameplayStatics, f);
        UE_LOGI("flashlight: click sound played at puppet pos (%.0f, %.0f, %.0f) "
                "attenuation=%p (sphere r=20m falloff=200m)",
                loc[0], loc[1], loc[2], sAttenuation);
    } else {
        UE_LOGW("flashlight: click sound NOT played -- asset=%p gs=%p fn=%p",
                sSoundAsset, sGameplayStatics, sPlaySoundFn);
    }
}

}  // namespace coop::flashlight_click_sound
