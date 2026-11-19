// ue_wrap/actors/prop_fire.cpp -- see ue_wrap/actors/prop_fire.h.

#include "ue_wrap/actors/prop_fire.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::prop {

bool PropFlameBaseValid(void* prop) {
    static reflection::InstanceOffset kFlameBase{L"flameBase"};
    const int32_t off = kFlameBase.Of(prop);
    if (off < 0) return false;
    void* flame = nullptr;
    std::memcpy(&flame, reinterpret_cast<const uint8_t*>(prop) + off, sizeof(flame));
    return flame && reflection::IsLive(flame);
}

namespace {

// The address of the prop's physicsImpact component's `health`; null when either is not there.
float* ImpactHealth(void* prop) {
    static reflection::InstanceOffset kImpact{L"physicsImpact"};
    static reflection::InstanceOffset kHealth{L"health"};
    const int32_t off = prop ? kImpact.Of(prop) : -1;
    if (off < 0) return nullptr;
    void* comp = nullptr;
    std::memcpy(&comp, reinterpret_cast<const uint8_t*>(prop) + off, sizeof(comp));
    if (!comp || !reflection::IsLive(comp)) return nullptr;
    const int32_t h = kHealth.Of(comp);
    return h < 0 ? nullptr : reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(comp) + h);
}

}  // namespace

bool ReadImpactHealth(void* prop, float& out) {
    const float* const p = ImpactHealth(prop);
    if (!p) return false;
    out = *p;
    return true;
}

bool WriteImpactHealth(void* prop, float health) {
    float* const p = ImpactHealth(prop);
    if (!p) return false;
    *p = health;
    return true;
}

bool CallAttemptIgnite(void* prop) {
    void* fn = reflection::FindDispatchFunctionCached(reflection::ClassOf(prop), L"attemptIgnite");
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && Call(prop, frame);
}

bool CallIgnite(void* prop, float fuel) {
    void* fn = reflection::FindDispatchFunctionCached(reflection::ClassOf(prop), L"ignite");
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && frame.Set(L"fuel", fuel) && Call(prop, frame);
}

}  // namespace ue_wrap::prop
