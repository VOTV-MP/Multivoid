// coop/player/skin_effects.cpp -- see skin_effects.h. The bytecode facts mirrored here, cited
// per function below: the kerfur's make-face deferred-spawns the face actor, stamps its type
// before finishing, and set-face slots the face's dynamic material into the mesh at the face
// material index; the face actor's begin-play builds its own render target and scene capture
// of its animated face mesh and leaves the screen material in its dynamic-material field
// (only the result is read here); the make-sentient add-ons (the glow material, the life
// particles, the life light) are not applied, since crafted kerfurs keep them off in their
// construction script and force-enabling them produced a pink blast, so the rig honours the
// template flags. The mynet variant's step calls the library step with volume zero (the
// default surface footstep muted), then spawns its emitter burst and plays its sound at the
// actor location with the default attenuation: replace mode. The base kerfur's step calls
// the library step at full volume (the default step audible), then layers the footstep
// sound scaled by the walk speed (clamped between half and double) as an attached sound at
// a quarter of that volume and a pitch of half that plus one, with the default attenuation:
// additive mode (the keljoy squeak).

#include "coop/player/skin_effects.h"

#include "coop/player/puppet_footsteps.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/scs_rig.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::skin_effects {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
namespace Pup = ue_wrap::puppet;

// The skin to variant-class map. The variant class is the game's own carrier of the skin's
// effect identity: its construction script adds the variant rig, and its default object
// holds the face material index, the type and the footstep sound. Meshes without a
// dedicated class (the maid dress, krampus) map to the base class, the kerfur body with the
// base life rig. The face flag gates the render-target face to the four omega bodies whose
// mesh has the screen slot (the maid mesh is single-slot; a default-object index alone is
// not mesh evidence).
struct Profile {
    const char* skin;
    const wchar_t* variantStem;  // /Game/objects/<stem>.<stem>_C
    bool allowFace;
    bool stepEmitter;  // mynet's eff_mynetEmitterStep burst
    // Replace step mode: the variant's step calls the library step with volume zero (the default
    // surface footstep muted) and plays its footstep sound itself at the actor location. False is
    // additive: the footstep sound layered by the stepped verb over the audible default.
    // Bytecode-derived per variant.
    bool stepReplace;
};
constexpr Profile kProfiles[] = {
    {"kerfur_omega",        L"kerfurOmega",              true,  false, false},
    {"kerfur_omega_h",      L"kerfurOmega_2",            true,  false, false},
    {"kerfur_omega_m",      L"kerfurOmega_1",            true,  false, false},
    {"kerfur_omega_nc",     L"kerfurOmega_0",            true,  false, false},
    {"kerfur_maid",         L"kerfurOmega",              false, false, false},
    {"kerfur_ariral",       L"kerfurOmega_ariral",       false, false, false},
    {"kerfur_ariral_suit",  L"kerfurOmega_ariral1",      false, false, false},
    {"kerfur_keljoy",       L"kerfurOmega_keljoy",       false, false, false},
    {"kerfur_mannequin",    L"kerfurOmega_mannequin",    false, false, false},
    {"skerfuro",            L"kerfurOmega_skerfuro",     false, false, false},
    {"scrappy_keith",       L"kerfurOmega_keith",        false, false, false},
    {"kerfur_antibreather", L"kerfurOmega_antibreather", false, false, false},
    {"kerfur_argplush",     L"kerfurOmega_argpl",        false, false, false},
    {"kerfur_alien",        L"kerfurOmega_alien",        false, false, false},
    {"kerfur_fleshly",      L"kerfurOmega_bonerman",     false, false, false},
    {"kerfur_skeleton",     L"kerfurOmega_bonerman1",    false, false, false},
    {"kerfur_vargskeleton", L"kerfurOmega_vargman",      false, false, false},
    {"kerfur_maxwell",      L"kerfurOmega_maxwell",      false, false, false},
    {"kerfur_erie",         L"kerfurOmega_erie",         false, false, false},
    {"kerfur_erie_v4",      L"kerfurOmega_erieV4",       false, false, false},
    {"kerfur_igetis",       L"kerfurOmega_igetis",       false, false, false},
    {"kerfur_monique",      L"kerfurOmega_monique",      false, false, false},
    {"kerfur_krampus",      L"kerfurOmega",              false, false, false},
    {"kerfur_mynet",        L"kerfurOmega_mynet",        false, true,  true},
    {"kerfur_furfur",       L"kerfurOmega_furfur",       false, false, false},
};

const Profile* FindProfile(const std::string& skin) {
    for (const auto& p : kProfiles)
        if (skin == p.skin) return &p;
    return nullptr;
}

// GC-safe cached asset and class loads (the client model's cached-asset shape, including its
// miss latch: a persistent miss must not re-run the path load, itself two full-array
// resolves, on every retry).
struct Cached {
    void* ptr = nullptr;
    int32_t idx = -1;
    bool tried = false;
};
std::map<std::wstring, Cached> g_loadCache;

void* LoadCached(const std::wstring& path) {
    Cached& c = g_loadCache[path];
    if (c.ptr && R::IsLiveByIndex(c.ptr, c.idx)) return c.ptr;
    if (c.ptr) c.tried = false;  // existed before, GC'd (level change) -- re-probe
    c.ptr = nullptr;
    if (c.tried) return nullptr;  // known-missing: stay silent + cheap
    c.tried = true;
    c.ptr = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
    c.idx = c.ptr ? R::InternalIndexOf(c.ptr) : -1;
    if (!c.ptr) UE_LOGW("skin_effects: load MISS '%ls' (latched -- game-version drift?)", path.c_str());
    return c.ptr;
}

void* LoadVariantClass(const wchar_t* stem) {
    std::wstring path = L"/Game/objects/";
    path += stem;
    path += L".";
    path += stem;
    path += L"_C";
    return LoadCached(path);
}

// The default-object field reads (declared on the base class; children share the layout).
int32_t g_offFaceIdx = -2, g_offType = -2, g_offFootstep = -2, g_offSkinMesh = -2;

void ResolveCdoOffsets(void* baseClass) {
    if (g_offFaceIdx != -2) return;
    g_offFaceIdx = R::FindPropertyOffset(baseClass, L"faceMaterialIndex");
    g_offType = R::FindPropertyOffset(baseClass, L"Type");
    g_offFootstep = R::FindPropertyOffset(baseClass, L"footstepSound");
    g_offSkinMesh = R::FindPropertyOffset(baseClass, L"skinMesh");
    if (g_offFaceIdx < 0 || g_offType < 0 || g_offFootstep < 0)
        UE_LOGW("skin_effects: kerfurOmega_C CDO offsets faceIdx=%d type=%d footstep=%d "
                "(game-version drift?)", g_offFaceIdx, g_offType, g_offFootstep);
}

template <typename T>
T ReadAt(void* obj, int32_t off, T fallback) {
    if (!obj || off < 0) return fallback;
    T v;
    std::memcpy(&v, reinterpret_cast<uint8_t*>(obj) + off, sizeof(T));
    return v;
}

// The per-body rig.
struct Rig {
    std::string skin;
    int32_t actorIdx = -1;
    void* faceActor = nullptr;
    int32_t faceIdx = -1;
    std::vector<std::pair<void*, int32_t>> comps;  // spawned cosmetic components
    void* stepSound = nullptr;    // kept alive by the variant class's CDO ref
    int32_t stepSoundIdx = -1;
    void* stepEmitter = nullptr;  // eff_mynetEmitterStep (mynet only)
    int32_t stepEmitterIdx = -1;
    bool stepReplace = false;     // Profile::stepReplace (mynet mutes the default)
    int32_t fmi = -1;  // face slot this rig overrode (cleared on teardown)
    coop::puppet_footsteps::Stride stride{};
    bool visible = true;
};
std::map<void*, Rig> g_rigs;

// Drop entries whose body actor is gone (a world change, respawn churn). Bounded by the peer
// count; the walk is trivial and runs only on Apply.
void SweepDeadRigs() {
    for (auto it = g_rigs.begin(); it != g_rigs.end();) {
        if (!R::IsLiveByIndex(it->first, it->second.actorIdx)) {
            // The body actor is already gone and its components died with it. The face actor is a
            // separate world actor: reap it if still live.
            Rig& r = it->second;
            if (r.faceActor && R::IsLiveByIndex(r.faceActor, r.faceIdx))
                E::DestroyActor(r.faceActor);
            it = g_rigs.erase(it);
        } else {
            ++it;
        }
    }
}

void TeardownRig(void* bodyActor, Rig& r, bool actorDying) {
    if (r.faceActor && R::IsLiveByIndex(r.faceActor, r.faceIdx))
        E::DestroyActor(r.faceActor);
    r.faceActor = nullptr;
    if (!actorDying) {
        for (auto& [comp, idx] : r.comps)
            if (R::IsLiveByIndex(comp, idx)) E::DestroyComponent(comp, bodyActor);
        // Clear the face-slot override, or the dead face actor's render-target material stays
        // painted on the next skin's mesh (the client model clears slot 0 only; the screen slot is
        // this rig's own write).
        if (r.fmi >= 0) {
            if (void* m = Pup::GetNativeBodyMeshComponent(bodyActor))
                E::SetComponentMaterial(m, r.fmi, nullptr);
            if (void* v = Pup::GetMeshPlayerVisibleComponent(bodyActor))
                E::SetComponentMaterial(v, r.fmi, nullptr);
        }
    }
    r.comps.clear();
}

// The make-face mirror: deferred-spawn the face class ten units up, set the type property by
// name, finish spawning; the face's begin-play generates the render target and the dynamic
// material. The material is read back and slotted at the face index.
void* SpawnFaceActor(int32_t faceType) {
    // The cached load resolves in memory first and caches, so no full-walk class find is needed.
    void* faceCls = LoadCached(L"/Game/objects/kerfusFace.kerfusFace_C");
    if (!faceCls) return nullptr;
    void* face = E::BeginDeferredSpawn(faceCls, ue_wrap::FVector{0.f, 0.f, 10.f},
                                       ue_wrap::FRotator{});
    if (!face) return nullptr;
    int32_t offType = R::FindPropertyOffset(faceCls, L"type");
    if (offType < 0) offType = R::FindPropertyOffset(faceCls, L"Type");
    if (offType >= 0)
        std::memcpy(reinterpret_cast<uint8_t*>(face) + offType, &faceType, sizeof(faceType));
    else
        UE_LOGW("skin_effects: kerfusFace type property not found -- face falls back to blue");
    if (!E::FinishDeferredSpawn(face, ue_wrap::FVector{0.f, 0.f, 10.f}, ue_wrap::FRotator{}))
        return nullptr;
    return face;
}

// Step effects (the emitter spawn, the firefly sync's resolve pattern).
void* g_gsCdo = nullptr;
void* g_spawnEmitterFn = nullptr;

bool ResolveEmitterAtLocation() {
    if (g_gsCdo && g_spawnEmitterFn) return true;
    g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo) {
        if (void* cls = R::ClassOf(g_gsCdo))
            g_spawnEmitterFn = R::FindFunction(cls, L"SpawnEmitterAtLocation");
    }
    return g_gsCdo && g_spawnEmitterFn;
}

// The mynet step burst: the emitter at the location, no rotation, unit scale, auto-destroy,
// no pooling, auto-activate.
void SpawnStepBurst(void* worldContext, void* emitter, const ue_wrap::FVector& loc) {
    if (!emitter || !ResolveEmitterAtLocation()) return;
    ue_wrap::ParamFrame f(g_spawnEmitterFn);
    f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"EmitterTemplate", emitter);
    f.SetRaw(L"Location", &loc, sizeof(loc));
    const ue_wrap::FRotator rot{};
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    const ue_wrap::FVector scale{1.f, 1.f, 1.f};
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);
    f.Set<bool>(L"bAutoActivateSystem", true);
    ue_wrap::Call(g_gsCdo, f);
}

// Remote-step loudness parity with the puppet footsteps' step volume.
constexpr float kStepFxVolume = 0.6f;

// The attached-sound spawn, the additive footstep layer's spawn.
void* g_soundAttachedFn = nullptr;

bool ResolveSoundAttached() {
    if (g_soundAttachedFn) return true;
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo)
        if (void* cls = R::ClassOf(g_gsCdo))
            g_soundAttachedFn = R::FindFunction(cls, L"SpawnSoundAttached");
    return g_soundAttachedFn != nullptr;
}

// The library step's speed-scaled loudness: the walk speed over 400, clamped between half
// and double, times the volume.
int32_t g_offCharMove = -2, g_offMaxWalk = -2;

float StepScaledVolume(void* bodyActor, float volume) {
    if (g_offCharMove == -2)
        g_offCharMove = R::FindPropertyOffset(R::ClassOf(bodyActor), L"CharacterMovement");
    float mws = 400.f;
    if (g_offCharMove >= 0) {
        if (void* move = ReadAt<void*>(bodyActor, g_offCharMove, nullptr)) {
            if (g_offMaxWalk == -2)
                g_offMaxWalk = R::FindPropertyOffset(R::ClassOf(move), L"MaxWalkSpeed");
            mws = ReadAt<float>(move, g_offMaxWalk, 400.f);
        }
    }
    float f = mws / 400.f;
    if (f < 0.5f) f = 0.5f;
    if (f > 2.f) f = 2.f;
    return f * volume;
}

// The stepped verb's attached-sound mirror: attached at the body (the native anchors the
// capsule; the mesh component is the same actor spot within the default attenuation's
// radius), not stopped on detach and auto-destroyed, as authored.
void SpawnStepSoundAttached(void* bodyActor, void* sound, void* att,
                            float vol, float pitch) {
    if (!ResolveSoundAttached()) return;
    void* anchor = Pup::GetNativeBodyMeshComponent(bodyActor);
    if (!anchor) return;
    ue_wrap::ParamFrame f(g_soundAttachedFn);
    f.Set<void*>(L"Sound", sound);
    f.Set<void*>(L"AttachToComponent", anchor);
    const ue_wrap::FVector loc{};
    const ue_wrap::FRotator rot{};
    f.SetRaw(L"Location", &loc, sizeof(loc));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.Set<uint8_t>(L"LocationType", 0);  // KeepRelativeOffset
    f.Set<bool>(L"bStopWhenAttachedToDestroyed", false);
    f.Set<float>(L"VolumeMultiplier", vol);
    f.Set<float>(L"PitchMultiplier", pitch);
    f.Set<float>(L"StartTime", 0.f);
    f.Set<void*>(L"AttenuationSettings", att);
    f.Set<void*>(L"ConcurrencySettings", nullptr);
    f.Set<bool>(L"bAutoDestroy", true);
    ue_wrap::Call(g_gsCdo, f);
}

// The shared step-effect dispatch behind OnStep (remote) and TickStride (local).
void StepFx(void* bodyActor, const ue_wrap::FVector& pos, bool localBody) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end()) return;
    Rig& r = it->second;
    const bool hasSound = r.stepSound && R::IsLiveByIndex(r.stepSound, r.stepSoundIdx);
    const bool hasBurst = r.stepEmitter && R::IsLiveByIndex(r.stepEmitter, r.stepEmitterIdx);
    if (!hasSound && !hasBurst) return;
    if (hasBurst) {
        // The burst at the skeleton root (the root bone sits between the feet, the closest stand-in
        // for the animation notify's foot contact point).
        ue_wrap::FVector feet = pos;
        if (void* mesh = Pup::GetNativeBodyMeshComponent(bodyActor))
            E::GetBoneWorldLocationByName(mesh, L"rootKerfur", feet);
        SpawnStepBurst(bodyActor, r.stepEmitter, feet);
    }
    if (!hasSound) return;
    // Native parity: both modes route through the default attenuation; without it a raw wave
    // plays in 2D over the whole map.
    void* att = LoadCached(L"/Game/audio/misc/att_default.att_default");
    if (r.stepReplace) {
        // Replace: the default step ran muted (volume zero via DefaultStepVolume) and the variant
        // plays its own sound at the actor location, flat volume and pitch. On the local body the
        // native default cannot be muted (its dispatch is invisible to the detour), so adding the
        // replacement would stack the exact double this mode removes; the sound layer is skipped.
        if (!localBody)
            E::PlaySoundAtLocation(bodyActor, r.stepSound, pos, att, 1.f, 1.f);
        return;
    }
    // Additive (keljoy): layered over the audible default with the native stepped math, fed the
    // same volume our puppet feeds the library step, so the native mix holds.
    const float scaled = StepScaledVolume(bodyActor, kStepFxVolume);
    SpawnStepSoundAttached(bodyActor, r.stepSound, att, scaled / 4.f, scaled / 2.f + 1.f);
}

}  // namespace

void Apply(void* bodyActor, const std::string& skinName) {
    if (!bodyActor) return;
    SweepDeadRigs();

    const Profile* prof = FindProfile(skinName);
    auto it = g_rigs.find(bodyActor);

    if (!prof) {
        // The kel and converter skins have no kerfur rig; tear down a previous one.
        if (it != g_rigs.end()) {
            UE_LOGI("skin_effects: '%s' has no effect rig -- removing previous ('%s')",
                    skinName.c_str(), it->second.skin.c_str());
            TeardownRig(bodyActor, it->second, /*actorDying=*/false);
            g_rigs.erase(it);
        }
        return;
    }

    if (it != g_rigs.end()) {
        Rig& r = it->second;
        // The same skin with the face actor (when one exists) still live: nothing to do. The
        // component list may legitimately be empty (a plain omega's base cosmetics are all dormant
        // sentient nodes), so emptiness is not a rebuild signal; components die only with the
        // actor, whose liveness the sweep above proved.
        const bool faceOk = !r.faceActor || R::IsLiveByIndex(r.faceActor, r.faceIdx);
        if (r.skin == skinName && faceOk) return;
        TeardownRig(bodyActor, r, /*actorDying=*/false);
        g_rigs.erase(it);
    }

    void* meshComp = Pup::GetNativeBodyMeshComponent(bodyActor);
    void* visComp = Pup::GetMeshPlayerVisibleComponent(bodyActor);
    if (!meshComp) meshComp = visComp;
    if (!meshComp) {
        UE_LOGW("skin_effects: '%s' on %p -- no body mesh component; rig skipped",
                skinName.c_str(), bodyActor);
        return;
    }

    void* baseClass = LoadVariantClass(L"kerfurOmega");
    if (!baseClass) return;  // load failure already logged
    ResolveCdoOffsets(baseClass);
    void* variantClass = LoadVariantClass(prof->variantStem);
    if (!variantClass) variantClass = baseClass;

    // The variant identity from the game's own default object (the face material index, the
    // type and the footstep sound are declared on the base class; children share the layout).
    void* cdo = nullptr;
    {
        std::wstring cdoName = L"kerfurOmega_C";
        if (variantClass != baseClass) {
            cdoName = prof->variantStem;
            cdoName += L"_C";
        }
        cdo = R::FindClassDefaultObject(cdoName.c_str());
    }
    const int32_t fmi = ReadAt<int32_t>(cdo, g_offFaceIdx, -1);
    const int32_t faceType = ReadAt<int32_t>(cdo, g_offType, 0);
    void* footstepSound = ReadAt<void*>(cdo, g_offFootstep, nullptr);

    Rig rig;
    rig.skin = skinName;
    rig.actorIdx = R::InternalIndexOf(bodyActor);

    // A census breadcrumb: the variant default object's own skin mesh should be a form of the
    // mesh this skin wears; a mismatch in the log means the profile table drifted from the game
    // version.
    if (void* sm = ReadAt<void*>(cdo, g_offSkinMesh, nullptr))
        UE_LOGI("skin_effects: skin '%s' <- variant '%ls' (CDO skinMesh '%ls')",
                skinName.c_str(), prof->variantStem,
                R::ToString(R::NameOf(sm)).c_str());

    // The construction-script cosmetic rig: the base class pass (the joint-life particles and the
    // belly light on every kerfur) plus the variant's own pass (the mynet electricity).
    std::vector<void*> comps;
    int made = ue_wrap::scs_rig::InstantiateCosmetics(bodyActor, meshComp, meshComp,
                                                      baseClass, comps);
    if (variantClass != baseClass)
        made += ue_wrap::scs_rig::InstantiateCosmetics(bodyActor, meshComp, meshComp,
                                                       variantClass, comps);
    rig.comps.reserve(comps.size());
    for (void* c : comps) rig.comps.emplace_back(c, R::InternalIndexOf(c));

    // The render-target face: the game's own face actor, its dynamic material slotted into the
    // mesh's screen slot (the make-face and set-face bytecode).
    if (prof->allowFace && fmi >= 0) {
        if (void* face = SpawnFaceActor(faceType)) {
            rig.faceActor = face;
            rig.faceIdx = R::InternalIndexOf(face);
            const int32_t offDyn = R::FindPropertyOffset(R::ClassOf(face), L"dynmat");
            if (void* dynmat = ReadAt<void*>(face, offDyn, nullptr)) {
                for (void* comp : {meshComp, visComp})
                    if (comp) E::SetComponentMaterial(comp, fmi, dynmat);
                rig.fmi = fmi;
            } else {
                UE_LOGW("skin_effects: kerfusFace %p spawned but dynmat is null "
                        "(gen() not run?) -- screen slot left raw", face);
            }
        }
    }

    // The step effect identity: the variant default object's footstep sound (the keljoy squeak,
    // the mynet bolt), the variant's step routing mode, and the mynet burst.
    if (footstepSound) {
        rig.stepSound = footstepSound;
        rig.stepSoundIdx = R::InternalIndexOf(footstepSound);
    }
    rig.stepReplace = prof->stepReplace;
    if (prof->stepEmitter) {
        if (void* em = LoadCached(L"/Game/particles/eff_mynetEmitterStep."
                                  L"eff_mynetEmitterStep")) {
            rig.stepEmitter = em;
            rig.stepEmitterIdx = R::InternalIndexOf(em);
        }
    }

    UE_LOGI("skin_effects: rig '%s' on %p -- %d SCS comp(s), face=%s (type=%d fmi=%d), "
            "stepSound=%s, stepBurst=%s",
            skinName.c_str(), bodyActor, made, rig.faceActor ? "YES" : "no",
            faceType, fmi, rig.stepSound ? "YES" : "no",
            rig.stepEmitter ? "YES" : "no");

    g_rigs.emplace(bodyActor, std::move(rig));
}

void OnBodyDestroyed(void* bodyActor) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end()) return;
    TeardownRig(bodyActor, it->second, /*actorDying=*/true);
    g_rigs.erase(it);
}

void SetRigVisible(void* bodyActor, bool visible) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end() || it->second.visible == visible) return;
    it->second.visible = visible;
    for (auto& [comp, idx] : it->second.comps)
        if (R::IsLiveByIndex(comp, idx))
            E::SetSceneComponentVisibility(comp, visible, /*propagate=*/true);
}

float DefaultStepVolume(void* bodyActor, float fallback) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end() || !it->second.stepReplace) return fallback;
    // Native replace parity: the library step still runs (the trace, water cues, friction), but
    // with volume zero the surface footstep is muted.
    return 0.f;
}

void OnStep(void* bodyActor, const ue_wrap::FVector& pos) {
    StepFx(bodyActor, pos, /*localBody=*/false);
}

void TickStride(void* bodyActor, const ue_wrap::FVector& pos, float speedCmS,
                bool grounded) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end()) return;
    Rig& r = it->second;
    // A cheap effect-presence check before the gate, so bodies without step effects cost a map
    // find only.
    if (!r.stepSound && !r.stepEmitter) return;
    if (r.stride.StepDue(pos, speedCmS, grounded))
        StepFx(bodyActor, pos, /*localBody=*/true);
}

}  // namespace coop::skin_effects
