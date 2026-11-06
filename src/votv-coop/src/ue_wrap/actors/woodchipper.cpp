// ue_wrap/actors/woodchipper.cpp -- see ue_wrap/actors/woodchipper.h.

#include "ue_wrap/actors/woodchipper.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::woodchipper {
namespace {

namespace R = reflection;

// The class as this world holds it, and its layout. The layout is the cooked class's, the same in every world, but it
// is read off the class object it was found on and asked again for another.
struct Layout {
    CachedObjRef cls;
    int32_t fuelOff = -1;
    int32_t activeOff = -1;
    uint8_t activeMask = 0;
    int32_t buttonOff = -1;
    bool ok = false;
    bool warned = false;
};
Layout g_layout;

void* ChipperClass() {
    if (void* c = g_layout.cls.Get()) return c;
    void* c = object_index::ClassByName(L"woodchipper_C");
    if (!c) return nullptr;
    g_layout = Layout{};
    g_layout.cls.Set(c);
    return c;
}

bool EnsureLayout() {
    void* cls = ChipperClass();
    if (!cls) return false;
    if (g_layout.ok) return true;
    g_layout.fuelOff = R::FindPropertyOffset(cls, L"fuel");
    g_layout.buttonOff = R::FindPropertyOffset(cls, L"button_del");
    int32_t off = -1;
    uint8_t mask = 0;
    if (R::FindBoolProperty(cls, L"active", off, mask) && off >= 0 && mask != 0) {
        g_layout.activeOff = off;
        g_layout.activeMask = mask;
    }
    g_layout.ok = g_layout.fuelOff >= 0 && g_layout.activeOff >= 0 && g_layout.buttonOff >= 0;
    if (!g_layout.ok && !g_layout.warned) {
        g_layout.warned = true;
        UE_LOGE("woodchipper: woodchipper_C did not resolve `fuel`, `active` or `button_del` (%d, %d, %d)",
                g_layout.fuelOff, g_layout.activeOff, g_layout.buttonOff);
    }
    return g_layout.ok;
}

}  // namespace

bool IsChipper(void* obj) {
    void* base = obj ? ChipperClass() : nullptr;
    void* cls = base ? R::ClassOf(obj) : nullptr;
    return cls && (cls == base || R::IsDescendantOfAny(cls, &base, 1));
}

size_t ForEachChipper(void (*fn)(void* ctx, void* chipper), void* ctx) {
    void* cls = fn ? ChipperClass() : nullptr;
    if (!cls) return 0;
    struct Thunk {
        void (*fn)(void*, void*);
        void* ctx;
    } thunk{fn, ctx};
    return object_index::ForEachInstance(cls, [](void* t, void* obj, int32_t) {
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        const Thunk& th = *static_cast<const Thunk*>(t);
        th.fn(th.ctx, obj);
    }, &thunk);
}

std::wstring KeyOf(void* chipper) {
    if (!IsChipper(chipper) || !R::IsLive(chipper)) return {};
    std::wstring key = prop::GetInteractableKeyString(chipper);
    return key == L"None" ? std::wstring() : key;
}

bool Read(void* chipper, float& fuel, bool& active) {
    if (!IsChipper(chipper) || !R::IsLive(chipper) || !EnsureLayout()) return false;
    const uint8_t* b = static_cast<const uint8_t*>(chipper);
    float f = 0.f;
    std::memcpy(&f, b + g_layout.fuelOff, sizeof(f));
    if (!std::isfinite(f)) return false;
    fuel = f;
    active = (b[g_layout.activeOff] & g_layout.activeMask) != 0;
    return true;
}

bool WriteFuel(void* chipper, float fuel) {
    if (!IsChipper(chipper) || !R::IsLive(chipper) || !EnsureLayout()) return false;
    std::memcpy(static_cast<uint8_t*>(chipper) + g_layout.fuelOff, &fuel, sizeof(fuel));
    return true;
}

bool WriteActive(void* chipper, bool active) {
    if (!IsChipper(chipper) || !R::IsLive(chipper) || !EnsureLayout()) return false;
    uint8_t& b = static_cast<uint8_t*>(chipper)[g_layout.activeOff];
    b = active ? static_cast<uint8_t>(b | g_layout.activeMask) : static_cast<uint8_t>(b & ~g_layout.activeMask);
    return true;
}

bool CallCheckActive(void* chipper) {
    return IsChipper(chipper) && R::IsLive(chipper) && component_calls::CallParamlessNamed(chipper, L"checkActive");
}

void* ButtonComponent(void* chipper) {
    if (!IsChipper(chipper) || !R::IsLive(chipper) || !EnsureLayout()) return nullptr;
    void* button = nullptr;
    std::memcpy(&button, static_cast<const uint8_t*>(chipper) + g_layout.buttonOff, sizeof(button));
    return button && R::IsLive(button) ? button : nullptr;
}

bool CallPress(void* chipper, void* player) {
    void* button = ButtonComponent(chipper);
    FVector at{};
    return button && player && engine::TryGetActorLocation(chipper, at) &&
           prop_use::CallActionOptionIndex(chipper, player, button, at, kButtonAction, button);
}

}  // namespace ue_wrap::woodchipper
