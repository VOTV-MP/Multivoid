// coop/prop_sound.cpp -- see coop/prop_sound.h.

#include "coop/props/prop_sound.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace coop::prop_sound {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The game's standard world-sound envelope -- the SAME asset the native
// pickup call passes (mainPlayer uber @100337 `obj<att_default>`), so using
// it IS native parity, not borrowing. Passing null would play the cue
// UNattenuated (audible map-wide). Resolved once; the asset is referenced by
// the always-loaded mainPlayer class so it stays live through gameplay.
// Fallback: a runtime-built default sphere (rooted by the wrapper) if the
// cooked asset is not found under this name.
void* ResolveAttenuation() {
    static void* sAtt = nullptr;
    if (!sAtt) sAtt = R::FindObject(L"att_default", L"SoundAttenuation");
    if (!sAtt) {
        ue_wrap::engine::SoundAttenuationConfig cfg{};  // default sphere 20m/200m
        sAtt = ue_wrap::engine::SpawnSoundAttenuation(cfg);
        if (sAtt) UE_LOGW("prop_sound: cooked att_default not found -- using a runtime default-sphere attenuation");
    }
    return sAtt;
}

// Which row of the material's Fstruct_physSound to pull.
enum class PhysCue { kSoft, kImpact };

// The FRESH DataTable lookup the local grab itself does (mainPlayer uber
// @100003: lib_C::physSound(hit.PhysMat)). The lookup's `return` bool is the
// @100067 row-miss gate: false -> NO sound, same as the local grabber hears
// for unmapped materials. Null physmat / unresolved lib -> null. `which`
// selects the row: kSoft = the grab pickup cue; kImpact = the land/impact thud.
void* CueFromPhysMat(void* physmat, void* worldCtx, PhysCue which) {
    if (!physmat || !R::IsLive(physmat)) return nullptr;
    static void* sLibCdo = nullptr;
    static void* sPhysSoundFn = nullptr;
    if (!sLibCdo || !sPhysSoundFn) {
        if (void* cls = R::FindClass(L"lib_C")) {
            sLibCdo = R::FindClassDefaultObject(L"lib_C");
            sPhysSoundFn = R::FindFunction(cls, L"physSound");
        }
    }
    if (!sLibCdo || !sPhysSoundFn) return nullptr;
    ue_wrap::ParamFrame f(sPhysSoundFn);
    f.Set<void*>(L"PhysMat", physmat);
    f.Set<void*>(L"__WorldContext", worldCtx);
    if (!ue_wrap::Call(sLibCdo, f) || !f.Get<bool>(L"return")) return nullptr;
    // Fstruct_physSound out-param head: step@0, impact@8, soft_30@0x10.
    struct PhysSoundHead { void* step; void* impact; void* soft; };
    const PhysSoundHead head = f.Get<PhysSoundHead>(L"Data");
    void* cue = (which == PhysCue::kImpact) ? head.impact : head.soft;
    return (cue && R::IsLive(cue)) ? cue : nullptr;
}

}  // namespace

void PlayGrabSound(void* propActor) {
    if (!propActor || !R::IsLive(propActor)) return;
    void* soft = nullptr;
    const char* path = nullptr;
    if (ue_wrap::prop::IsDescendantOfProp(propActor)) {
        // prop_C path: physicsImpact @0x0230 caches both the material row and the
        // PhysMat. Tier 1 (fast): the CACHED physSoundData.soft_30. Hands-on
        // 2026-06-11 falsified "filled at init": a never-impacted prop's cache
        // can be EMPTY (it fills lazily), so a null cache is NOT a row miss --
        // tier 2 redoes the fresh lookup from the component's own PhysMat @0x0290.
        void* impactComp = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(propActor) + P::off::Aprop_physicsImpact);
        if (!impactComp || !R::IsLive(impactComp)) {
            UE_LOGI("prop_sound: grab cue SKIP -- physicsImpact null/dead (prop=%p)", propActor);
            return;
        }
        soft = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(impactComp) +
            P::off::CompPhysImpact_physSoundData + P::off::FstructPhysSound_soft);
        path = "cache";
        if (!soft || !R::IsLive(soft)) {
            path = "fresh";
            void* physmat = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(impactComp) + P::off::CompPhysImpact_PhysMat);
            soft = CueFromPhysMat(physmat, propActor, PhysCue::kSoft);
        }
    } else {
        // Generic-actor path (hands-on 2026-06-11: the trash clump derives from
        // plain Actor DESPITE the prop_ name -- pile/clump grabs were silent on
        // peers while the grabber natively hears the garbage soft cue). Resolve
        // the physmat the way the grabber's trace did -- root surface material ->
        // physical material -- and feed the same physSound lookup.
        path = "root-mat";
        soft = CueFromPhysMat(
            ue_wrap::engine::GetActorRootPhysicalMaterial(propActor), propActor, PhysCue::kSoft);
    }
    if (!soft || !R::IsLive(soft)) {
        UE_LOGI("prop_sound: grab cue SKIP -- no soft cue (%s miss; actor=%p)",
                path, propActor);
        return;
    }
    void* att = ResolveAttenuation();
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(propActor);
    // Native: vol 0.5 / pitch 1.0 at hit.Location + Normal*10; the prop's own
    // location is the faithful receiver-side stand-in for the grab point.
    ue_wrap::engine::PlaySoundAtLocation(propActor, soft, loc, att, 0.5f, 1.0f);
    UE_LOGI("prop_sound: grab cue (%s) at (%.0f, %.0f, %.0f)", path, loc.X, loc.Y, loc.Z);
}

void PlayUseClick(void* propActor) {
    if (!propActor || !R::IsLive(propActor)) return;
    // SoundWave /Game/audio/effects/use -- THE fixed E-action feedback. Native:
    // useAction plays PlaySound2D(use, 0.25, 1.0) on the GRABBER only, right
    // after pickupObject (useAction @2181/@305/@3156/@3337 -- every successful
    // E branch ends in it). It is the "always the same single sound" of a grab;
    // 2D collector-only, so peers natively hear nothing -- this spatializes it
    // at the grabbed prop (the throw-whoosh synthesis pattern, user-driven
    // 2026-06-11). Volume 0.5 not 0.25: the native figure is in-ear 2D; a world
    // source rolls off through att_default, so 0.5 lands a bystander at grab-
    // watching range near the grabber's perceived loudness.
    static void* sUse = nullptr;
    if (!sUse) {
        sUse = R::FindObject(L"use", P::name::SoundWaveClass);
        if (!sUse) sUse = R::FindObject(L"use", L"SoundCue");
    }
    if (!sUse) {
        UE_LOGW("prop_sound: 'use' sound asset not resolved yet -- no use click");
        return;
    }
    void* att = ResolveAttenuation();
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(propActor);
    ue_wrap::engine::PlaySoundAtLocation(propActor, sUse, loc, att, 0.5f, 1.0f);
    UE_LOGI("prop_sound: use click at (%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
}

void PlayThrowWhoosh(void* propActor) {
    if (!propActor || !R::IsLive(propActor)) return;
    // SoundWave /Game/audio/effects/swing -- THE native throw whoosh, played
    // by BOTH throw variants (@16057 / @16826). Resolve once, retry while the
    // asset hasn't streamed in; SoundCue fallback just in case.
    static void* sSwing = nullptr;
    if (!sSwing) {
        sSwing = R::FindObject(L"swing", P::name::SoundWaveClass);
        if (!sSwing) sSwing = R::FindObject(L"swing", L"SoundCue");
    }
    if (!sSwing) {
        UE_LOGW("prop_sound: 'swing' sound asset not resolved yet -- no whoosh");
        return;
    }
    void* att = ResolveAttenuation();
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(propActor);
    // Native: PlaySound2D(swing, 0.8, 1.05) on the thrower only. Spatializing
    // at the released prop is the receiver-side synthesis (deliberate
    // improvement -- natively NOBODY else hears a throw).
    ue_wrap::engine::PlaySoundAtLocation(propActor, sSwing, loc, att, 0.8f, 1.05f);
    UE_LOGI("prop_sound: swing whoosh at (%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
}

void PlayLandSound(void* propActor) {
    if (!propActor || !R::IsLive(propActor)) return;
    // The pile's landed-material IMPACT cue (physSound.impact) -- the sibling of
    // the grab's soft cue. A pile is a plain actor (not Aprop_C), so resolve the
    // physmat the same way the generic-actor grab path does: root surface
    // material -> physical material -> the physSound row. Silent on a row miss
    // (native parity, the @100067 gate). RE (2026-07-01): the clump->pile
    // conversion + the pile's BeginPlay/init play NO explicit sound; the native
    // "thud" is this material impact.
    void* impact = CueFromPhysMat(
        ue_wrap::engine::GetActorRootPhysicalMaterial(propActor), propActor, PhysCue::kImpact);
    if (!impact || !R::IsLive(impact)) {
        UE_LOGI("prop_sound: land thud SKIP -- no impact cue (root-mat miss; pile=%p)", propActor);
        return;
    }
    void* att = ResolveAttenuation();
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(propActor);
    // Native flesh_impact PlaySoundAtLocation params: vol 1.0 / pitch 1.0 / att_default.
    ue_wrap::engine::PlaySoundAtLocation(propActor, impact, loc, att, 1.0f, 1.0f);
    UE_LOGI("prop_sound: pile land thud at (%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
}

void PlayDenyClick(void* playerActor) {
    if (!playerActor || !R::IsLive(playerActor)) return;
    // SoundWave /Game/audio/effects/button_keypad_deny -- THE game's own
    // save-denied/failed click (analogDScreenTest uber @23927 plays it
    // PlaySound2D vol 0.5 / pitch 1.0 on the play-screen save deny; the
    // keypad deny is the same asset). v63 device occupancy plays it on the
    // peer whose E-press hit a busy device (user design: "deny + the
    // existing fail sound"). Spatialized AT the denied player = full volume
    // in their own ears (distance ~0), matching the native 2D loudness.
    static void* sDeny = nullptr;
    if (!sDeny) {
        sDeny = R::FindObject(L"button_keypad_deny", P::name::SoundWaveClass);
        if (!sDeny) sDeny = R::FindObject(L"button_keypad_deny", L"SoundCue");
    }
    if (!sDeny) {
        UE_LOGW("prop_sound: 'button_keypad_deny' not resolved yet -- no deny click");
        return;
    }
    void* att = ResolveAttenuation();
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(playerActor);
    ue_wrap::engine::PlaySoundAtLocation(playerActor, sDeny, loc, att, 0.5f, 1.0f);
    UE_LOGI("prop_sound: deny click at (%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
}

void PlayInventoryBlipAt(void* worldCtx, const ue_wrap::FVector& loc) {
    if (!worldCtx || !R::IsLive(worldCtx)) return;
    // SoundCue /Game/audio/effects/inventory_Cue -- THE native collect blip
    // (putObjectInventory2 @659 plays it PlaySound2D vol 1.0 / pitch 1.1).
    // Resident on every peer via mainPlayer's import table; latched resolve.
    static void* sCue = nullptr;
    if (!sCue) {
        sCue = R::FindObject(L"inventory_Cue", L"SoundCue");
        if (!sCue) sCue = R::FindObject(L"inventory_Cue", P::name::SoundWaveClass);
    }
    if (!sCue) {
        UE_LOGW("prop_sound: 'inventory_Cue' not resolved yet -- no pickup blip");
        return;
    }
    void* att = ResolveAttenuation();
    ue_wrap::engine::PlaySoundAtLocation(worldCtx, sCue, loc, att, 1.0f, 1.1f);
    UE_LOGI("prop_sound: inventory blip at (%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
}

}  // namespace coop::prop_sound
