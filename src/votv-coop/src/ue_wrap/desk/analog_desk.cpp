// ue_wrap/desk/analog_desk.cpp -- see ue_wrap/desk/analog_desk.h.

#include "ue_wrap/desk/analog_desk.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstring>
#include <string>

namespace ue_wrap::analog_desk {
namespace {

namespace R  = reflection;
namespace OI = object_index;

constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
const wchar_t* const kPlateClass[kPlates] = {L"prop_computerBackPanel_tl_C", L"prop_computerBackPanel_bl_C",
                                             L"prop_computerBackPanel_tr_C", L"prop_computerBackPanel_br_C"};
const wchar_t* const kLid[kPlates] = {L"lid1", L"lid2", L"lid3_no", L"lid4"};

R::InstanceOffset g_state{L"panelsState"};
R::InstanceOffset g_unscrew{L"panelsUnscrew"};
R::InstanceOffset g_panels{L"panels"};
R::InstanceOffset g_looking{L"lookingAtPanel"};
R::InstanceOffset g_lid[kPlates] = {R::InstanceOffset{kLid[0]}, R::InstanceOffset{kLid[1]},
                                    R::InstanceOffset{kLid[2]}, R::InstanceOffset{kLid[3]}};

// A Blueprint bool is an FBoolProperty: its byte offset and the bit inside that byte, found once per class object.
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
};
BoolField g_process{L"panelsProcess"};
BoolField g_takeoff{L"panelTakeoff"};

bool ResolveBool(void* obj, BoolField& f) {
    void* const cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return false;
    if (f.cls != cls) {
        f.cls = cls;
        f.off = -1;
        f.mask = 0;
        if (!R::FindBoolProperty(cls, f.name, f.off, f.mask) || f.off < 0 || !f.mask) f.off = -1;
    }
    return f.off >= 0;
}

const field_io::TArrayView* ArrayOf(void* desk, R::InstanceOffset& off) {
    const int32_t o = desk ? off.Of(desk) : -1;
    return o >= 0 ? reinterpret_cast<const field_io::TArrayView*>(static_cast<const char*>(desk) + o) : nullptr;
}

bool Short(const field_io::TArrayView* on, const field_io::TArrayView* loose) {
    return !on || !loose || !on->data || !loose->data || on->num < kPlates || loose->num < kPlates;
}

void* MemberObj(void* desk, R::InstanceOffset& off) {
    const int32_t o = desk ? off.Of(desk) : -1;
    return o >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(desk) + o) : nullptr;
}

}  // namespace

void* Find() {
    void* const cls = OI::ClassByName(kDeskClass);
    if (!cls) return nullptr;
    void* found = nullptr;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        void*& f = *static_cast<void**>(ctx);
        if (!f && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) f = obj;
    }, &found);
    return found;
}

bool ReadPlates(void* desk, uint8_t& bits) {
    const auto* on = ArrayOf(desk, g_state);
    const auto* loose = ArrayOf(desk, g_unscrew);
    if (Short(on, loose)) return false;
    bits = 0;
    for (int i = 0; i < kPlates; ++i) {
        if (on->data[i]) bits |= static_cast<uint8_t>(1u << i);
        if (loose->data[i]) bits |= static_cast<uint8_t>(1u << (4 + i));
    }
    return true;
}

bool WritePlateArrays(void* desk, uint8_t bits) {
    auto* on = const_cast<field_io::TArrayView*>(ArrayOf(desk, g_state));
    auto* loose = const_cast<field_io::TArrayView*>(ArrayOf(desk, g_unscrew));
    if (Short(on, loose)) return false;
    for (int i = 0; i < kPlates; ++i) {
        on->data[i] = ((bits >> i) & 1u) ? 1 : 0;
        loose->data[i] = ((bits >> (4 + i)) & 1u) ? 1 : 0;
    }
    return true;
}

bool CallUpdPanels(void* desk) { return desk && component_calls::CallParamlessNamed(desk, L"updPanels"); }

bool CallSoundDetach(void* desk, const FVector& at) {
    ParamFrame f(desk ? R::FindDispatchFunctionCached(R::ClassOf(desk), L"soundDetachPanel") : nullptr);
    return f.valid() && f.Set<FVector>(L"location", at) && Call(desk, f);
}

void* PlateMesh(void* desk, int i) {
    const auto* panels = ArrayOf(desk, g_panels);
    if (!panels || !panels->data || i < 0 || i >= panels->num) return nullptr;
    void* const mesh = reinterpret_cast<void* const*>(panels->data)[i];
    return mesh && R::IsLive(mesh) ? mesh : nullptr;
}

void* Lid(void* desk, int i) { return i >= 0 && i < kPlates ? MemberObj(desk, g_lid[i]) : nullptr; }

const wchar_t* PlatePropClass(int i) { return i >= 0 && i < kPlates ? kPlateClass[i] : nullptr; }

int PlateOfProp(void* actor) {
    if (!actor) return -1;
    const std::wstring cls = R::ClassNameOf(actor);
    for (int i = 0; i < kPlates; ++i)
        if (_wcsicmp(cls.c_str(), kPlateClass[i]) == 0) return i;
    return -1;
}

bool ProcessOr(void* desk, bool fallback) {
    if (!ResolveBool(desk, g_process)) return fallback;
    return (static_cast<const uint8_t*>(desk)[g_process.off] & g_process.mask) != 0;
}

bool WriteTakeoff(void* desk, bool value) {
    if (!ResolveBool(desk, g_takeoff)) return false;
    uint8_t& b = static_cast<uint8_t*>(desk)[g_takeoff.off];
    b = value ? static_cast<uint8_t>(b | g_takeoff.mask) : static_cast<uint8_t>(b & ~g_takeoff.mask);
    return true;
}

int32_t LookingAtPanel(void* desk) {
    const int32_t o = desk ? g_looking.Of(desk) : -1;
    if (o < 0) return -1;
    int32_t v = -1;
    std::memcpy(&v, static_cast<const char*>(desk) + o, sizeof(v));
    return v;
}

}  // namespace ue_wrap::analog_desk
