// ue_wrap/engine/engine_audio.cpp -- positional sound spawning.
//
// Engine-wrapper layer (principle 7). The USoundAttenuation construct and config, plus the
// UGameplayStatics::PlaySoundAtLocation dispatch. Declared in ue_wrap/engine/engine_audio.h
// (SoundAttenuationConfig / SpawnSoundAttenuation / PlaySoundAtLocation), which the engine.h
// umbrella includes, so a caller reaches these through either header.

#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <vector>
#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// Cached UFunction pointers + UClass for SpawnSoundAttenuation. The 3
// resolve attempts run lazily on first call; once non-null they stay
// (GameplayStatics CDO + SoundAttenuation UClass are process-stable).
void* g_gsCdoForAtt    = nullptr;
void* g_spawnObjectFn  = nullptr;
void* g_attClass       = nullptr;

bool ResolveAttSpawn() {
    if (!g_gsCdoForAtt) g_gsCdoForAtt = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdoForAtt && !g_spawnObjectFn) {
        if (void* gsCls = R::ClassOf(g_gsCdoForAtt)) {
            g_spawnObjectFn = R::FindFunction(gsCls, P::name::SpawnObjectFn);
        }
    }
    if (!g_attClass) g_attClass = R::FindClass(P::name::SoundAttenuationClass);
    return g_gsCdoForAtt && g_spawnObjectFn && g_attClass;
}
}  // namespace

void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg) {
    if (!ResolveAttSpawn()) return nullptr;

    // 1) SpawnObject(objectClass, Outer) -> UObject*. CASE-SENSITIVE param
    //    names per UE reflection: lowercase 'objectClass' + 'Outer' (the
    //    existing pattern in engine_widget.cpp's widget spawn). An
    //    uppercase-O `ObjectClass` would FName-mismatch -> SetRaw fail ->
    //    SpawnObject sees a null class -> returns null. Outer = the
    //    GameplayStatics CDO, which is process-stable.
    void* obj = nullptr;
    {
        ParamFrame f(g_spawnObjectFn);
        f.Set<void*>(L"objectClass", g_attClass);
        f.Set<void*>(L"Outer", g_gsCdoForAtt);
        if (!Call(g_gsCdoForAtt, f)) return nullptr;
        obj = f.Get<void*>(L"ReturnValue");
    }
    if (!obj) return nullptr;

    // 2) Configure via raw memory writes. UE exposes no setter
    //    UFunctions for these fields (they are edit-time UProperties
    //    on USoundAttenuation). Offsets cataloged in sdk_profile.h
    //    `att::` namespace -- the wrapper hides them from gameplay code.
    auto* p = reinterpret_cast<uint8_t*>(obj);
    *reinterpret_cast<uint8_t*>(p + P::off::att::AttenuationShape)  = cfg.shape;
    *reinterpret_cast<uint8_t*>(p + P::off::att::DistanceAlgorithm) = cfg.distanceAlgorithm;
    *reinterpret_cast<uint8_t*>(p + P::off::att::FalloffMode)       = cfg.falloffMode;
    float* extents = reinterpret_cast<float*>(p + P::off::att::AttenuationShapeExtents);
    extents[0] = cfg.extents[0];
    extents[1] = cfg.extents[1];
    extents[2] = cfg.extents[2];
    *reinterpret_cast<float*>(p + P::off::att::FalloffDistance)    = cfg.falloffDistance;
    *reinterpret_cast<float*>(p + P::off::att::ConeOffset)         = cfg.coneOffset;
    *reinterpret_cast<float*>(p + P::off::att::dBAttenuationAtMax) = cfg.dBAttenuationAtMax;
    uint8_t& flags = *reinterpret_cast<uint8_t*>(p + P::off::att::FlagsByte);
    if (cfg.attenuate)  flags |= 0x01; else flags &= ~static_cast<uint8_t>(0x01);
    if (cfg.spatialize) flags |= 0x02; else flags &= ~static_cast<uint8_t>(0x02);

    // 3) AddToRoot so UE GC keeps this object alive across collections. The caller holds the
    //    pointer in a C++ static, which UE's reachability scan cannot see, so without rooting the
    //    object is reaped on the next GC pass and the next PlaySoundAtLocation reads freed memory.
    //    PROCESS-LIFETIME by design: the attenuation object is outered to the GameplayStatics CDO
    //    rather than to a world, so it is not a world anchor and is never released. It is still
    //    held through an OWNED pin rather than a bare flag write, so it shows up in the pin
    //    registry instead of being invisible to it.
    static std::vector<ue_wrap::GcPin> sPermanentPins;
    sPermanentPins.emplace_back(obj);
    return obj;
}

void PlaySoundAtLocation(void* worldContext, void* sound, const FVector& location,
                         void* attenuation, float volume, float pitch) {
    if (!worldContext || !sound) return;
    // UGameplayStatics::PlaySoundAtLocation CDO + UFunction, cached once.
    static void* sGsCdo  = nullptr;
    static void* sPlayFn = nullptr;
    if (!sGsCdo) sGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (!sPlayFn && sGsCdo) {
        if (void* c = R::ClassOf(sGsCdo)) sPlayFn = R::FindFunction(c, P::name::PlaySoundAtLocationFn);
    }
    if (!sGsCdo || !sPlayFn) return;
    // Non-cone source -> orientation unused; pass a zero rotator. Tolerates a
    // null attenuation (plays 2D in that case). The per-call transient
    // UAudioComponent is engine-managed (auto-destroy at playback end).
    const FRotator rot{};
    ParamFrame f(sPlayFn);
    f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"Sound", sound);
    f.SetRaw(L"Location", &location, sizeof(location));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.Set<float>(L"VolumeMultiplier", volume);
    f.Set<float>(L"PitchMultiplier", pitch);
    f.Set<float>(L"StartTime", 0.f);
    f.Set<void*>(L"AttenuationSettings", attenuation);
    f.Set<void*>(L"ConcurrencySettings", nullptr);
    f.Set<void*>(L"OwningActor", worldContext);
    Call(sGsCdo, f);
}

}  // namespace ue_wrap::engine
