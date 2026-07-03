// coop/player/skin_effects.cpp -- see skin_effects.h. RE ground truth:
// research/pak_re dump_precise.py / dump_scs.py / census_variants.py over the
// kerfurOmega family + kerfusFace (2026-07-03). Key bytecode facts mirrored
// here, cited per function below:
//   kerfurOmega.makeFace : deferred-spawn kerfusFace_C at (0,0,10), stamp int
//                          'type' pre-Finish, setFace() -> Mesh.SetMaterial(
//                          faceMaterialIndex, face.dynmat).
//   kerfusFace.ReceiveBeginPlay -> gen(): builds its own 256x256 RT +
//                          scene-capture of its AnimBP'd face mesh; leaves the
//                          screen MID in `dynmat` (we only read the result).
//   kerfurOmega.makeSentient : the SENTIENT-only add-ons ('ag' glow MID, 14x
//                          eff_life*.Activate, lifeLight visible). NOT applied
//                          here: crafted kerfurs keep them OFF (bAutoActivate/
//                          bVisible=false in the SCS), and force-enabling them
//                          was the 2026-07-03 "pink blast" user regression --
//                          scs_rig now honors the template flags.
//   kerfurOmega_mynet.step (ubergraph @3-@6): lib_C::step(self, ..., volume=0,
//                          ...) -- the DEFAULT surface footstep is MUTED -- then
//                          SpawnEmitterAtLocation(eff_mynetEmitterStep, loc) +
//                          PlaySoundAtLocation(boltrix_mediumHit, ActorLocation,
//                          vol 1, pitch 1, att_default). REPLACE mode.
//   kerfurOmega.step: lib_C::step(self, ..., volume=1, ...) -- default step
//                          audible; stepped(scaled) then layers footstepSound:
//                          scaled = clamp(MaxWalkSpeed/400, 0.5, 2) * volume,
//                          SpawnSoundAttached(footstepSound, capsule, vol
//                          scaled/4, pitch scaled/2+1, att_default). ADDITIVE
//                          mode (keljoy squeak).

#include "coop/player/skin_effects.h"

#include "coop/player/puppet_footsteps.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/asset_load.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/scs_rig.h"
#include "ue_wrap/sdk_profile.h"

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

// ---- skin -> variant class map (census_variants.py 2026-07-03) -------------
// The variant class is the game's own carrier of the skin's effect identity:
// its SCS adds the variant rig, its CDO holds faceMaterialIndex / Type /
// footstepSound. Meshes without a dedicated class (maid dress, krampus) map to
// the BASE class: kerfur body => base life rig. allowFace gates the RT face to
// the four omega bodies whose MESH has the screen slot (KerfurO_maid measured
// single-slot 2026-07-03 -- a CDO fmi alone is not mesh evidence).
struct Profile {
    const char* skin;
    const wchar_t* variantStem;  // /Game/objects/<stem>.<stem>_C
    bool allowFace;
    bool stepEmitter;  // mynet's eff_mynetEmitterStep burst
    // REPLACE step mode: the variant's step() calls lib_C::step with volume 0
    // (default surface footstep muted) and plays footstepSound itself at the
    // actor location. false = ADDITIVE (footstepSound layered by stepped()
    // over the audible default). Bytecode-derived per variant.
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

// ---- GC-safe cached asset/class loads (client_model's CachedAsset shape,
// incl. its miss latch: a persistent MISS must not re-run LoadObjectByPath --
// itself two full-array resolves -- on every retry; perf audit W1) -----------
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

// ---- CDO field reads (declared on the BASE class; layout shared by children).
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

// ---- the per-body rig -------------------------------------------------------
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

// Drop entries whose body actor is gone (world change / respawn churn). Bounded
// by the peer count -- the walk is trivial and runs only on Apply.
void SweepDeadRigs() {
    for (auto it = g_rigs.begin(); it != g_rigs.end();) {
        if (!R::IsLiveByIndex(it->first, it->second.actorIdx)) {
            // The body actor is already gone -- its components died with it.
            // The face actor is a SEPARATE world actor: reap it if still live.
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
        // Clear the face-slot override, or the dead face actor's RT material
        // stays painted on the next skin's mesh (client_model clears slot 0
        // only -- the screen slot is this rig's own write).
        if (r.fmi >= 0) {
            if (void* m = Pup::GetNativeBodyMeshComponent(bodyActor))
                E::SetComponentMaterial(m, r.fmi, nullptr);
            if (void* v = Pup::GetMeshPlayerVisibleComponent(bodyActor))
                E::SetComponentMaterial(v, r.fmi, nullptr);
        }
    }
    r.comps.clear();
}

// ---- makeFace mirror --------------------------------------------------------
// kerfurOmega.makeFace bytecode: BeginDeferred(kerfusFace_C, Z=10) ->
// SetIntPropertyByName('type', Type) -> FinishSpawning; kerfusFace BeginPlay
// gen()s the RT + dynmat. We read `dynmat` back and slot it at fmi.
void* SpawnFaceActor(int32_t faceType) {
    // LoadCached resolves in-memory first (StaticLoadObject) and caches --
    // no FindClass full walk needed (perf audit W3).
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

// ---- step FX (SpawnEmitterAtLocation, firefly_sync's resolve pattern) -------
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

// mynet step burst (ubergraph @4): SpawnEmitterAtLocation(eff_mynetEmitterStep,
// loc, rot 0, scale 1, autoDestroy, poolMethod none, autoActivate).
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

// Remote-step loudness parity with puppet_footsteps::Stride::kStepVolume.
constexpr float kStepFxVolume = 0.6f;

// GameplayStatics::SpawnSoundAttached -- the ADDITIVE footstep layer's spawn
// (kerfurOmega stepped region [1266]).
void* g_soundAttachedFn = nullptr;

bool ResolveSoundAttached() {
    if (g_soundAttachedFn) return true;
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo)
        if (void* cls = R::ClassOf(g_gsCdo))
            g_soundAttachedFn = R::FindFunction(cls, L"SpawnSoundAttached");
    return g_soundAttachedFn != nullptr;
}

// lib_C::step's speed-scaled loudness (bytecode [60]-[62]):
// clamp(CharacterMovement.MaxWalkSpeed / 400, 0.5, 2) * volume.
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

// stepped()'s SpawnSoundAttached mirror ([1266]): attached at the body (native
// anchors the capsule; the mesh component is the same actor spot within
// att_default's radius), bStop=false / autoDestroy=true exactly as authored.
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

// The shared step-FX dispatch behind OnStep (remote) and TickStride (local).
void StepFx(void* bodyActor, const ue_wrap::FVector& pos, bool localBody) {
    auto it = g_rigs.find(bodyActor);
    if (it == g_rigs.end()) return;
    Rig& r = it->second;
    const bool hasSound = r.stepSound && R::IsLiveByIndex(r.stepSound, r.stepSoundIdx);
    const bool hasBurst = r.stepEmitter && R::IsLiveByIndex(r.stepEmitter, r.stepEmitterIdx);
    if (!hasSound && !hasBurst) return;
    if (hasBurst) {
        // Burst at the skeleton root (rootKerfur sits between the feet -- the
        // closest stand-in for the anim notify's foot contact point).
        ue_wrap::FVector feet = pos;
        if (void* mesh = Pup::GetNativeBodyMeshComponent(bodyActor))
            E::GetBoneWorldLocationByName(mesh, L"rootKerfur", feet);
        SpawnStepBurst(bodyActor, r.stepEmitter, feet);
    }
    if (!hasSound) return;
    // Native parity: both modes route through att_default (mynet step @6 /
    // stepped [1266]) -- without it a raw wave plays 2D on the whole map.
    void* att = LoadCached(L"/Game/audio/misc/att_default.att_default");
    if (r.stepReplace) {
        // REPLACE (mynet step @4-@6): the default step ran muted (volume 0 via
        // DefaultStepVolume); the variant plays its own sound at the ACTOR
        // location, flat volume/pitch 1. On the LOCAL body the native default
        // cannot be muted (EX-invisible) -- adding the replacement would stack
        // the exact double this mode removes, so the sound layer is skipped.
        if (!localBody)
            E::PlaySoundAtLocation(bodyActor, r.stepSound, pos, att, 1.f, 1.f);
        return;
    }
    // ADDITIVE (keljoy): layered over the audible default with the native
    // stepped() math, fed the same volume our puppet feeds lib step so the
    // native mix holds.
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
        // dr_kel / converter skins: no kerfur rig. Tear down a previous one.
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
        // Same skin + the face actor (when one exists) still live => nothing to
        // do. comps may legitimately be EMPTY (take-2: a plain omega's base SCS
        // cosmetics are all dormant sentient nodes), so emptiness is not a
        // rebuild signal; components die only with the actor, whose liveness
        // the sweep above already proved.
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

    // Variant identity from the game's own CDO (faceMaterialIndex / Type /
    // footstepSound are declared on the base class; children share the layout).
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

    // Census breadcrumb: the variant CDO's own skinMesh should be (a form of)
    // the mesh this skin wears -- a mismatch in the log means the profile
    // table drifted from the game version.
    if (void* sm = ReadAt<void*>(cdo, g_offSkinMesh, nullptr))
        UE_LOGI("skin_effects: skin '%s' <- variant '%ls' (CDO skinMesh '%ls')",
                skinName.c_str(), prof->variantStem,
                R::ToString(R::NameOf(sm)).c_str());

    // 1) The SCS cosmetic rig: base class pass (joint-life particles + belly
    //    light on every kerfur) + the variant's own pass (mynet electricity).
    std::vector<void*> comps;
    int made = ue_wrap::scs_rig::InstantiateCosmetics(bodyActor, meshComp, meshComp,
                                                      baseClass, comps);
    if (variantClass != baseClass)
        made += ue_wrap::scs_rig::InstantiateCosmetics(bodyActor, meshComp, meshComp,
                                                       variantClass, comps);
    rig.comps.reserve(comps.size());
    for (void* c : comps) rig.comps.emplace_back(c, R::InternalIndexOf(c));

    // 2) The RT face -- the game's own kerfusFace actor, dynmat into the
    //    mesh's screen slot (makeFace/setFace bytecode).
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

    // 3) Step FX identity: the variant CDO's footstepSound (keljoy squeak,
    //    mynet boltrix) + the variant's step routing mode + mynet's burst.
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
    // Native REPLACE parity (mynet step @3): lib_C::step still runs -- trace,
    // water cues, friction -- but with volume 0 the surface footstep is muted.
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
    // Cheap FX presence check BEFORE the gate so bodies without step FX cost
    // a map find only (the gate itself is float math, but why prime it).
    if (!r.stepSound && !r.stepEmitter) return;
    if (r.stride.StepDue(pos, speedCmS, grounded))
        StepFx(bodyActor, pos, /*localBody=*/true);
}

}  // namespace coop::skin_effects
