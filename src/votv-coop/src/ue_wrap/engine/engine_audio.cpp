// ue_wrap/engine/engine_audio.cpp -- positional sound spawning.
//
// Engine-wrapper layer (principle 7). The USoundAttenuation construct and config, plus the
// UGameplayStatics::PlaySoundAtLocation dispatch. Declared in ue_wrap/engine/engine_audio.h
// (SoundAttenuationConfig / SpawnSoundAttenuation / PlaySoundAtLocation), which the engine.h
// umbrella includes, so a caller reaches these through either header.

#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "coop/text/i18n.h"

#include <unordered_map>
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

    // 1) SpawnObject(objectClass, Outer) -> UObject*. The param names are spelled as UE
    //    reflection renders them, lowercase 'objectClass' + 'Outer' (the existing pattern in
    //    engine_widget.cpp's widget spawn); ParamFrame::OffsetOf matches insensitively, so the
    //    casing is documentation and not a match condition. Outer = the GameplayStatics CDO,
    //    which is process-stable.
    void* obj = nullptr;
    {
        ParamFrame f(g_spawnObjectFn);
        f.Set<void*>(L"objectClass", g_attClass);
        f.Set<void*>(L"Outer", g_gsCdoForAtt);
        if (!Call(g_gsCdoForAtt, f)) return nullptr;
        obj = f.Get<void*>(coop::i18n::TrW(L"ReturnValue"));
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

void PlaySound2D(void* worldContext, void* sound, float volume, float pitch, float startTime, bool uiSound) {
    if (!worldContext || !sound) return;
    static void* sGsCdo = nullptr;
    static void* sPlayFn = nullptr;
    if (!sGsCdo) sGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (!sPlayFn && sGsCdo) {
        if (void* c = R::ClassOf(sGsCdo)) sPlayFn = R::FindFunction(c, L"PlaySound2D");
    }
    if (!sGsCdo || !sPlayFn) return;
    ParamFrame f(sPlayFn);
    f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"Sound", sound);
    f.Set<float>(L"VolumeMultiplier", volume);
    f.Set<float>(L"PitchMultiplier", pitch);
    f.Set<float>(L"StartTime", startTime);
    f.Set<void*>(L"ConcurrencySettings", nullptr);
    f.Set<void*>(L"OwningActor", nullptr);
    f.Set<bool>(L"bIsUISound", uiSound);
    Call(sGsCdo, f);
}

std::wstring SoundName(void* sound) {
    void* cls = sound ? R::ClassOf(sound) : nullptr;
    void* package = sound ? R::OuterOf(sound) : nullptr;
    if (!cls || !package || R::OuterOf(package)) return {};
    return R::ToString(R::NameOf(cls)) + L" " + R::ToString(R::NameOf(package)) + L"." +
           R::ToString(R::NameOf(sound));
}

namespace {

struct SoundQuery {
    const wchar_t* package;
    const wchar_t* leaf;
    void*          found;
};

void MatchSound(void* ctx, void* obj, int32_t) {
    auto* q = static_cast<SoundQuery*>(ctx);
    if (q->found || !R::NameEquals(R::NameOf(obj), q->leaf)) return;
    void* package = R::OuterOf(obj);
    if (package && !R::OuterOf(package) && R::NameEquals(R::NameOf(package), q->package)) q->found = obj;
}

}  // namespace

void* FindSound(const std::wstring& name) {
    static std::unordered_map<std::wstring, CachedObjRef> sKnown;
    auto it = sKnown.find(name);
    if (it != sKnown.end() && it->second.Alive()) return it->second.Get();
    const size_t space = name.find(L' ');
    const size_t dot = name.rfind(L'.');
    if (space == std::wstring::npos || dot == std::wstring::npos || dot < space) return nullptr;
    const std::wstring className = name.substr(0, space);
    const std::wstring package = name.substr(space + 1, dot - space - 1);
    const std::wstring leaf = name.substr(dot + 1);
    void* cls = object_index::ClassByName(className.c_str());
    if (!cls) return nullptr;
    SoundQuery q{package.c_str(), leaf.c_str(), nullptr};
    object_index::ForEachInstance(cls, &MatchSound, &q);
    if (q.found) sKnown[name].Set(q.found);
    return q.found;
}

}  // namespace ue_wrap::engine
