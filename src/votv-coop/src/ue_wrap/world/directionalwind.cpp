// ue_wrap/world/directionalwind.cpp -- see ue_wrap/world/directionalwind.h.

#include "ue_wrap/world/directionalwind.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::directionalwind {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// The wind actor, destroyed and recreated with each level. Game thread.
void* Resolve() { return world_singleton::Find(P::name::DirectionalWindClass); }

}  // namespace

bool Read(WindState& out) {
    void* w = Resolve();
    if (!w || !R::IsLive(w)) return false;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(w);
    out.speedBg      = *reinterpret_cast<const float*>(b + P::off::DirectionalWind_windSpeed_background);
    out.strengthBg   = *reinterpret_cast<const float*>(b + P::off::DirectionalWind_windStrength_background);
    out.speedRain    = *reinterpret_cast<const float*>(b + P::off::DirectionalWind_windSpeed_rain);
    out.strengthRain = *reinterpret_cast<const float*>(b + P::off::DirectionalWind_windStrength_rain);
    return true;
}

bool Write(const WindState& in) {
    void* w = Resolve();
    if (!w || !R::IsLive(w)) return false;
    uint8_t* b = reinterpret_cast<uint8_t*>(w);
    *reinterpret_cast<float*>(b + P::off::DirectionalWind_windSpeed_background)    = in.speedBg;
    *reinterpret_cast<float*>(b + P::off::DirectionalWind_windStrength_background) = in.strengthBg;
    *reinterpret_cast<float*>(b + P::off::DirectionalWind_windSpeed_rain)          = in.speedRain;
    *reinterpret_cast<float*>(b + P::off::DirectionalWind_windStrength_rain)       = in.strengthRain;
    return true;
}

// windTarget is a UBillboardComponent ptr @ DirectionalWind_windTarget; the gust input is its
// USceneComponent::RelativeLocation, at the sdk_profile.h offset. ReadTarget and WriteTarget both
// chase the component ptr then the field -- a null windTarget (mid-init) returns false.
namespace {
uint8_t* WindTargetComp(void* wind) {
    return *reinterpret_cast<uint8_t**>(
        reinterpret_cast<uint8_t*>(wind) + P::off::DirectionalWind_windTarget);
}
}  // namespace

bool ReadTarget(FVector& out) {
    void* w = Resolve();
    if (!w || !R::IsLive(w)) return false;
    uint8_t* tgt = WindTargetComp(w);
    if (!tgt) return false;
    out = *reinterpret_cast<FVector*>(tgt + P::off::USceneComponent_RelativeLocation);
    return true;
}

bool WriteTarget(const FVector& in) {
    void* w = Resolve();
    if (!w || !R::IsLive(w)) return false;
    uint8_t* tgt = WindTargetComp(w);
    if (!tgt) return false;
    *reinterpret_cast<FVector*>(tgt + P::off::USceneComponent_RelativeLocation) = in;
    return true;
}

}  // namespace ue_wrap::directionalwind
