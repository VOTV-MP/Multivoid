// ue_wrap/actors/food.cpp -- see ue_wrap/actors/food.h.

#include "ue_wrap/actors/food.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstring>
#include <unordered_map>

namespace ue_wrap::food {
namespace {

namespace R = reflection;

constexpr float kKelvinToCelsius = 273.f;  // the tick's own constant, not 273.15
constexpr float kSteamAboveCelsius = 50.f;
constexpr float kFrostOnMesh = 0.5f;
constexpr int32_t kFrostDataIndex = 1;

// Everything asked of a class once: whether it is a food (prop_food_C or a child of it) and where its fields live. A
// subclass has its own offsets, so two classes live at once must not share one slot. A class a world reload frees can
// come back at the address of another, so the table goes with the world (SyncWorld), as prop_can_state's does.
struct ClassInfo {
    bool    food = false;
    int32_t temperature = -1, ripeness = -1, uses = -1, celsius = -1, steam = -1, impact = -1;
    int32_t cook = -1;        // prop_cookingFood_C's cookProgress; -1 on any other food
    int32_t frozenOff = -1;   // tempLessThan0 is a Blueprint bool: one bit of a byte it may share with its neighbours
    uint8_t frozenMask = 0;
};
std::unordered_map<void*, ClassInfo> g_class;
uint32_t g_world = 0;   // the world_identity generation the table belongs to

void SyncWorld() {
    // Generation() bumps only when CurrentWorld() is observed to change, so look here first (world_identity.h:55-58).
    world_identity::CurrentWorld();
    const uint32_t gen = world_identity::Generation();
    if (gen == g_world) return;
    g_world = gen;
    g_class.clear();
}

ClassInfo& InfoOf(void* cls) {
    SyncWorld();
    auto it = g_class.find(cls);
    if (it != g_class.end()) return it->second;
    ClassInfo info;
    for (void* c = cls; c && !info.food; c = R::SuperStructOf(c))
        info.food = R::NameEquals(R::NameOf(c), L"prop_food_C");
    if (info.food) {
        info.temperature = R::FindPropertyOffset(cls, L"temperature");
        info.ripeness = R::FindPropertyOffset(cls, L"ripeness");
        info.uses = R::FindPropertyOffset(cls, L"uses");
        info.celsius = R::FindPropertyOffset(cls, L"celsius");
        info.steam = R::FindPropertyOffset(cls, L"eff_steam");
        info.impact = R::FindPropertyOffset(cls, L"physicsImpact");
        info.cook = R::FindPropertyOffset(cls, L"cookProgress");
        if (!R::FindBoolProperty(cls, L"tempLessThan0", info.frozenOff, info.frozenMask) || !info.frozenMask) {
            info.frozenOff = -1;
            info.frozenMask = 0;
        }
        UE_LOGI("food: class %ls: temperature @0x%X ripeness @0x%X uses @0x%X celsius @0x%X eff_steam @0x%X "
                "physicsImpact @0x%X tempLessThan0 @0x%X mask 0x%02X",
                R::ToString(R::NameOf(cls)).c_str(), info.temperature, info.ripeness, info.uses, info.celsius,
                info.steam, info.impact, info.frozenOff, info.frozenMask);
        if (info.temperature < 0 || info.ripeness < 0 || info.uses < 0 || info.celsius < 0)
            UE_LOGW("food: class %ls declares no temperature, ripeness, uses or celsius -- its foods are not read",
                    R::ToString(R::NameOf(cls)).c_str());
    }
    return g_class.emplace(cls, info).first->second;
}

void* ReadObjectField(int32_t off, void* actor) {
    if (off < 0) return nullptr;
    void* value = nullptr;
    std::memcpy(&value, static_cast<const uint8_t*>(actor) + off, sizeof(value));
    return value && R::IsLive(value) ? value : nullptr;
}

bool ComponentActive(void* component) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(component), L"IsActive");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(component, f) && f.Get<bool>(L"ReturnValue");
}

bool FrozenField(const ClassInfo& info, void* actor, uint8_t*& byte, uint8_t& mask) {
    if (info.frozenOff < 0 || !info.frozenMask) return false;
    byte = static_cast<uint8_t*>(actor) + info.frozenOff;
    mask = info.frozenMask;
    return true;
}

void PaintFrozen(const ClassInfo& info, void* actor, bool frozen) {
    if (void* mesh = prop::GetStaticMesh(actor)) {
        if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"SetCustomPrimitiveDataFloat")) {
            ParamFrame f(fn);
            if (f.valid() && f.Set<int32_t>(L"DataIndex", kFrostDataIndex) &&
                f.Set<float>(L"Value", frozen ? kFrostOnMesh : 0.f))
                Call(mesh, f);
        }
    }
    if (void* impact = ReadObjectField(info.impact, actor)) {
        static R::InstanceOffset sOverride{L"physmat_override"};
        const int32_t off = sOverride.Of(impact);
        if (off >= 0) {
            void* material = frozen ? R::FindObject(L"concrete", L"PhysicalMaterial") : nullptr;
            std::memcpy(static_cast<uint8_t*>(impact) + off, &material, sizeof(material));
        }
        component_calls::CallParamlessNamed(impact, L"makePhysSound");
    }
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"freezed")) {
        ParamFrame f(fn);
        if (f.valid() && f.Set<bool>(L"frozen", frozen)) Call(actor, f);
    }
}

}  // namespace

bool IsFoodClass(void* cls) { return cls && InfoOf(cls).food; }

bool IsFood(void* actor) { return actor && R::IsLive(actor) && IsFoodClass(R::ClassOf(actor)); }

bool Read(void* actor, Condition& out) {
    if (!IsFood(actor)) return false;
    const ClassInfo& info = InfoOf(R::ClassOf(actor));
    if (info.temperature < 0 || info.ripeness < 0 || info.uses < 0) return false;
    const auto* base = static_cast<const uint8_t*>(actor);
    std::memcpy(&out.temperature, base + info.temperature, sizeof(float));
    std::memcpy(&out.ripeness, base + info.ripeness, sizeof(float));
    std::memcpy(&out.uses, base + info.uses, sizeof(int32_t));
    out.cookProgress = -1.f;
    if (info.cook >= 0) std::memcpy(&out.cookProgress, base + info.cook, sizeof(float));
    return true;
}

bool Write(void* actor, const Condition& in) {
    if (!IsFood(actor)) return false;
    const ClassInfo& info = InfoOf(R::ClassOf(actor));
    if (info.temperature < 0 || info.ripeness < 0 || info.uses < 0 || info.celsius < 0) return false;
    auto* base = static_cast<uint8_t*>(actor);
    const float c = in.temperature - kKelvinToCelsius;
    std::memcpy(base + info.temperature, &in.temperature, sizeof(float));
    std::memcpy(base + info.ripeness, &in.ripeness, sizeof(float));
    std::memcpy(base + info.uses, &in.uses, sizeof(int32_t));
    std::memcpy(base + info.celsius, &c, sizeof(float));
    if (info.cook >= 0 && in.cookProgress >= 0.f) std::memcpy(base + info.cook, &in.cookProgress, sizeof(float));
    return true;
}

bool PaintLooks(void* actor) {
    Condition now;
    if (!Read(actor, now)) return false;
    const ClassInfo& info = InfoOf(R::ClassOf(actor));
    const float celsius = now.temperature - kKelvinToCelsius;
    if (void* steam = ReadObjectField(info.steam, actor)) {
        const bool want = celsius > kSteamAboveCelsius;
        if (ComponentActive(steam) != want) {
            if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(steam), L"SetActive")) {
                ParamFrame f(fn);
                if (f.valid() && f.Set<bool>(L"bNewActive", want) && f.Set<bool>(L"bReset", true)) Call(steam, f);
            }
        }
    }
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (!FrozenField(info, actor, byte, mask)) return true;
    const bool frozen = celsius <= 0.f;
    if (((*byte & mask) != 0) != frozen) {
        *byte = frozen ? (*byte | mask) : (*byte & ~mask);
        PaintFrozen(info, actor, frozen);
    }
    return true;
}

bool ReadLooks(void* actor, bool& steam, bool& frozen) {
    if (!IsFood(actor)) return false;
    const ClassInfo& info = InfoOf(R::ClassOf(actor));
    void* const component = ReadObjectField(info.steam, actor);
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (!component || !FrozenField(info, actor, byte, mask)) return false;
    steam = ComponentActive(component);
    frozen = (*byte & mask) != 0;
    return true;
}

}  // namespace ue_wrap::food
