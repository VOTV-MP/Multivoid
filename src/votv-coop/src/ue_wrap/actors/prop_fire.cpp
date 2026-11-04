// ue_wrap/actors/prop_fire.cpp -- see ue_wrap/actors/prop_fire.h.

#include "ue_wrap/actors/prop_fire.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
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

namespace {

void* Flame(void* prop) {
    static reflection::InstanceOffset kFlameBase{L"flameBase"};
    const int32_t off = prop ? kFlameBase.Of(prop) : -1;
    if (off < 0) return nullptr;
    void* flame = nullptr;
    std::memcpy(&flame, reinterpret_cast<const uint8_t*>(prop) + off, sizeof(flame));
    return flame && reflection::IsLive(flame) ? flame : nullptr;
}

template <class T>
bool ReadField(void* obj, const wchar_t* name, T& out) {
    const int32_t off = reflection::FindPropertyOffset(reflection::ClassOf(obj), name);
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = reflection::FindPropertyOffset(reflection::ClassOf(obj), name);
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
    return true;
}

bool ReadBurning(void* flame, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!reflection::FindBoolProperty(reflection::ClassOf(flame), L"isBurning", off, mask) || off < 0 || !mask) return false;
    out = (static_cast<const uint8_t*>(flame)[off] & mask) != 0;
    return true;
}

}  // namespace

bool ReadPropFire(void* prop, FireState& out) {
    out = FireState{};
    void* flame = Flame(prop);
    if (!flame) return true;
    if (!ReadBurning(flame, out.burning)) return false;
    if (!out.burning) return true;
    if (!ReadField(flame, L"fuel", out.fuel) || !ReadField(flame, L"fuelSpeed", out.fuelSpeed)) return false;
    if (out.fuel <= 0.f) out = FireState{};   // its own end leaves a spent fuel for a frame before the flame goes
    return true;
}

void ListBurningProps(std::vector<void*>& out) {
    out.clear();
    void* cls = object_index::ClassByName(L"comp_flammable_C");
    if (!cls) return;
    object_index::ForEachInstance(cls, [](void* ctx, void* flame, int32_t) {
        void* owner = nullptr;
        if (!reflection::IsLive(flame) || !ReadField(flame, L"owner", owner) || !owner || !reflection::IsLive(owner)) return;
        if (!IsDescendantOfProp(owner) || Flame(owner) != flame) return;
        FireState s;
        if (ReadPropFire(owner, s) && s.burning) static_cast<std::vector<void*>*>(ctx)->push_back(owner);
    }, &out);
}

bool CallExtinguish(void* prop) {
    void* flame = Flame(prop);
    if (!flame) return true;
    void* fn = reflection::FindDispatchFunctionCached(reflection::ClassOf(flame), L"extinguish");
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && Call(flame, frame);
}

bool ApplyPropFire(void* prop, const FireState& state) {
    if (!state.burning) return CallExtinguish(prop);
    FireState now;
    if (!ReadPropFire(prop, now)) return false;
    if (!now.burning) {
        void* base = object_index::ClassByName(L"prop_C");
        void* fn = base ? reflection::FindDispatchFunctionCached(base, L"ignite") : nullptr;
        if (!fn) return false;
        ParamFrame frame(fn);
        if (!frame.valid() || !frame.Set(L"fuel", state.fuel) || !Call(prop, frame)) return false;
    }
    void* flame = Flame(prop);
    return flame && WriteField(flame, L"fuel", state.fuel) && WriteField(flame, L"fuelSpeed", state.fuelSpeed);
}

}  // namespace ue_wrap::prop
