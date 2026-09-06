// ue_wrap/engine/engine_audio.h -- playing a sound at a world location, with the attenuation the
// game uses. Engine-wrapper layer (principle 7): each call marshals one UFunction call or one
// reflected field access, with no gameplay, network or coop state. Game thread unless a
// declaration says otherwise. Implementation: src/ue_wrap/engine/engine_audio.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::engine {

// USoundAttenuation config; the enum values follow UE4.27's EAttenuationShape / DistanceModel /
// FalloffMode, and the defaults are the flashlight-click tuning.
struct SoundAttenuationConfig {
    uint8_t shape              = 0;        // 0=Sphere, 1=Box, 2=Capsule, 3=Cone
    uint8_t distanceAlgorithm  = 2;        // 0=Linear, 1=Logarithmic, 2=Inverse, ...
    uint8_t falloffMode        = 0;        // 0=Continues, 1=Hold
    float   extents[3]         = {2000.f, 0.f, 0.f};  // sphere: extents[0] = radius (cm)
    float   falloffDistance    = 20000.f;             // cm beyond extents until silence
    float   coneOffset         = 0.f;
    float   dBAttenuationAtMax = -60.f;
    bool    attenuate          = true;     // FlagsByte bit 0
    bool    spatialize         = true;     // FlagsByte bit 1
};

// Construct a USoundAttenuation (SpawnObject plus raw field writes; the fields have no setters) and
// AddToRoot it: callers keep the pointer in a C++ static, invisible to UE's GC, and an unrooted
// object would be reaped under PlaySoundAtLocation. nullptr on failure. Game thread.
void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg);

// UGameplayStatics::PlaySoundAtLocation: a one-shot 3D sound at `location` owned by `worldContext`;
// `attenuation` may be null (2D). Game thread.
void PlaySoundAtLocation(void* worldContext, void* sound, const FVector& location,
                         void* attenuation, float volume = 1.f, float pitch = 1.f);

}  // namespace ue_wrap::engine
