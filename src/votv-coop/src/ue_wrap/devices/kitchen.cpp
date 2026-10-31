// ue_wrap/devices/kitchen.cpp -- see ue_wrap/devices/kitchen.h.

#include "ue_wrap/devices/kitchen.h"

#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstring>
#include <unordered_map>

namespace ue_wrap::kitchen {
namespace {

namespace R = reflection;

constexpr float kMicrowaveSeconds[kModes] = {5.f, 10.f, 15.f, 30.f, 60.f};  // its press's own table

std::unordered_map<void*, Kind> g_kinds;

R::InstanceOffset g_timeMode{L"timeMode"};
R::InstanceOffset g_timer{L"timer"};
R::InstanceOffset g_time{L"time"};
R::InstanceOffset g_mode{L"mode"};
R::InstanceOffset g_coals{L"coals"};
R::InstanceOffset g_burn{L"a"};

template <class T>
bool Get(R::InstanceOffset& field, void* actor, T& out) {
    const int32_t off = field.Of(actor);
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(actor) + off, sizeof(T));
    return true;
}

template <class T>
bool Put(R::InstanceOffset& field, void* actor, const T& value) {
    const int32_t off = field.Of(actor);
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(actor) + off, &value, sizeof(T));
    return true;
}

bool Bit(void* actor, const wchar_t* name, uint8_t*& byte, uint8_t& mask) {
    int32_t off = -1;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0 || !mask) return false;
    byte = static_cast<uint8_t*>(actor) + off;
    return true;
}

bool ActiveBit(void* actor, uint8_t*& byte, uint8_t& mask) { return Bit(actor, L"active", byte, mask); }

}  // namespace

Kind KindOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return Kind::None;
    void* const cls = R::ClassOf(actor);
    if (!cls) return Kind::None;
    auto it = g_kinds.find(cls);
    if (it != g_kinds.end()) return it->second;
    Kind kind = Kind::None;
    for (void* c = cls; c && kind == Kind::None; c = R::SuperStructOf(c)) {
        if (R::NameEquals(R::NameOf(c), kMicrowaveClass)) kind = Kind::Microwave;
        else if (R::NameEquals(R::NameOf(c), kMiniOvenClass)) kind = Kind::MiniOven;
        else if (R::NameEquals(R::NameOf(c), kGrillClass)) kind = Kind::Grill;
    }
    g_kinds.emplace(cls, kind);
    return kind;
}

bool Read(void* actor, State& out) {
    const Kind kind = KindOf(actor);
    if (kind == Kind::None) return false;
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (!ActiveBit(actor, byte, mask)) return false;
    State value;
    value.kind = kind;
    value.active = (*byte & mask) != 0;
    if (kind == Kind::Microwave) {
        if (!Get(g_timeMode, actor, value.mode) || !Get(g_time, actor, value.remaining)) return false;
    } else if (kind == Kind::Grill) {
        uint8_t* lid = nullptr;
        uint8_t lidMask = 0;
        if (!Get(g_coals, actor, value.mode) || !Get(g_burn, actor, value.remaining) ||
            !Bit(actor, L"hasLid", lid, lidMask))
            return false;
        value.lid = (*lid & lidMask) != 0;
    } else if (!Get(g_mode, actor, value.mode)) {
        return false;
    }
    out = value;
    return true;
}

bool ApplyMode(void* actor, int32_t mode) {
    const Kind kind = KindOf(actor);
    if (kind == Kind::Grill) return mode >= 0 && mode <= kMaxCoals && Put(g_coals, actor, mode);
    if (mode < 0 || mode >= kModes) return false;
    if (kind == Kind::Microwave) return Put(g_timeMode, actor, mode) && Put(g_timer, actor, kMicrowaveSeconds[mode]);
    if (kind != Kind::MiniOven || !Put(g_mode, actor, mode)) return false;
    return component_calls::CallParamlessNamed(actor, L"upd");
}

bool ApplyActive(void* actor, bool active, float remaining) {
    const Kind kind = KindOf(actor);
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (kind == Kind::None || !ActiveBit(actor, byte, mask)) return false;
    const bool now = (*byte & mask) != 0;
    if (kind == Kind::Microwave) {
        if (!active) return !now || Put(g_time, actor, 0.f);
        if (!now && !component_calls::CallParamlessNamed(actor, L"Wave")) return false;
        return remaining <= 0.f || Put(g_time, actor, remaining);
    }
    if (kind == Kind::Grill) Put(g_burn, actor, remaining);
    if (kind == Kind::Grill && now == active) return true;  // its repaint restarts its fire
    *byte = active ? (*byte | mask) : (*byte & ~mask);
    return component_calls::CallParamlessNamed(actor, L"upd");
}

bool ApplyLid(void* actor, bool lid) {
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (KindOf(actor) != Kind::Grill || !Bit(actor, L"hasLid", byte, mask)) return false;
    if (((*byte & mask) != 0) == lid) return true;
    *byte = lid ? (*byte | mask) : (*byte & ~mask);
    return component_calls::CallParamlessNamed(actor, L"updLid");
}

}  // namespace ue_wrap::kitchen
