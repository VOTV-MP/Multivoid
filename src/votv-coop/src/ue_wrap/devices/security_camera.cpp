// ue_wrap/devices/security_camera.cpp -- see ue_wrap/devices/security_camera.h. The class, the two bits'
// offsets and masks are resolved from the live class by name; a name that does not resolve leaves the
// camera out, said once, since a guessed offset would write whatever a newer build keeps there.

#include "ue_wrap/devices/security_camera.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

namespace ue_wrap::security_camera {
namespace {

namespace R = reflection;

constexpr const wchar_t* kBaseClass   = L"prop_camera_bad_C";
constexpr const wchar_t* kCursedClass = L"prop_camera_cursed_C";
constexpr const wchar_t* kWidgetClass = L"ui_laptop_C";

// A BP bool: its byte and the mask of its bit. The class layout, the same in every world, so it is read once.
struct Bit { int32_t off = -1; uint8_t mask = 0; };

CachedObjRef g_base;     // the base class as this world holds it
CachedObjRef g_cursed;
Bit  g_active, g_light;
bool g_bitsResolved = false;
bool g_bitsUnusable = false;  // the class loaded and a bit did not resolve: said once, left out

// A class kept only while its slot and serial still hold it (a Blueprint class can get a new object in a
// new world), looked up by name again otherwise: one object-index lookup, no walk.
void* ClassFor(CachedObjRef& ref, const wchar_t* name) {
    if (void* c = ref.Get()) return c;
    void* c = object_index::ClassByName(name);
    if (c) ref.Set(c);
    return c;
}

bool ReadBit(void* obj, const Bit& b) {
    return (*(reinterpret_cast<const uint8_t*>(obj) + b.off) & b.mask) != 0;
}

void WriteBit(void* obj, const Bit& b, bool on) {
    uint8_t& byte = *(reinterpret_cast<uint8_t*>(obj) + b.off);
    byte = on ? static_cast<uint8_t>(byte | b.mask) : static_cast<uint8_t>(byte & ~b.mask);
}

// The key each camera class carries by default, read from its default object once per class object.
struct DefaultKey { CachedObjRef cls; std::wstring key; };
constexpr int kMaxDefaults = 4;
DefaultKey g_defaults[kMaxDefaults];
int g_defaultCount = 0;  // entries filled, up to kMaxDefaults
int g_defaultNext = 0;   // the entry the next class takes

std::wstring DefaultKeyOf(void* cam) {
    void* cls = R::ClassOf(cam);
    for (int i = 0; i < g_defaultCount; ++i)
        if (g_defaults[i].cls.Is(cls)) return g_defaults[i].key;
    // The class's default object is among its listed instances: found there, with no walk of the object array.
    void* cdo = nullptr;
    object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* out = static_cast<void**>(ctx);
            if (!*out && R::NameStartsWith(R::NameOf(obj), L"Default__")) *out = obj;
        },
        &cdo);
    if (!cdo) return std::wstring();  // not listed (yet): asked again, never cached as an empty default
    std::wstring key = prop::GetKeyString(cdo);
    // Four camera classes are the most a cook holds; a full table, or an entry a world reload killed, is
    // overwritten round-robin.
    DefaultKey& slot = g_defaults[g_defaultNext];
    g_defaultNext = (g_defaultNext + 1) % kMaxDefaults;
    if (g_defaultCount < kMaxDefaults) ++g_defaultCount;
    slot.cls.Set(cls);
    slot.key = key;
    return key;
}

R::InstanceOffset g_widgetCam{L"cam"};  // ui_laptop_C::cam

}  // namespace

bool EnsureResolved() {
    if (g_bitsResolved) return true;
    if (g_bitsUnusable) return false;
    void* cls = ClassFor(g_base, kBaseClass);
    if (!cls) return false;
    Bit active, light;
    if (!R::FindBoolProperty(cls, L"active", active.off, active.mask) ||
        !R::FindBoolProperty(cls, L"isLight", light.off, light.mask) || active.off < 0 || light.off < 0) {
        g_bitsUnusable = true;
        UE_LOGE("security_camera: %ls.active / .isLight did not resolve by name (active@%d isLight@%d) -- the "
                "cameras are left out of the sync", kBaseClass, active.off, light.off);
        return false;
    }
    g_active = active;
    g_light = light;
    g_bitsResolved = true;
    UE_LOGI("security_camera: resolved %ls active@0x%04X/0x%02X isLight@0x%04X/0x%02X", kBaseClass,
            static_cast<unsigned>(active.off), static_cast<unsigned>(active.mask),
            static_cast<unsigned>(light.off), static_cast<unsigned>(light.mask));
    return true;
}

bool IsCameraClass(void* obj) {
    if (!obj) return false;
    void* base = ClassFor(g_base, kBaseClass);
    void* cls = base ? R::ClassOf(obj) : nullptr;
    return cls && R::IsDescendantOfAny(cls, &base, 1);
}

bool IsCursed(void* obj) {
    if (!obj) return false;
    void* cursed = ClassFor(g_cursed, kCursedClass);
    void* cls = cursed ? R::ClassOf(obj) : nullptr;
    return cls && R::IsDescendantOfAny(cls, &cursed, 1);
}

std::wstring KeyOf(void* cam) {
    if (!IsCameraClass(cam)) return std::wstring();
    std::wstring key = prop::GetKeyString(cam);
    if (key.empty() || key == L"None") return std::wstring();
    if (key == DefaultKeyOf(cam)) return std::wstring();
    return key;
}

bool IsCamera(void* obj) { return IsCameraClass(obj) && !KeyOf(obj).empty(); }

bool ReadActive(void* cam, bool& on) {
    if (!cam || !EnsureResolved() || !IsCameraClass(cam)) return false;
    on = ReadBit(cam, g_active);
    return true;
}

bool ReadLight(void* cam, bool& on) {
    if (!cam || !EnsureResolved() || !IsCameraClass(cam)) return false;
    on = ReadBit(cam, g_light);
    return true;
}

bool WriteActive(void* cam, bool on) {
    if (!cam || !EnsureResolved() || !IsCameraClass(cam)) return false;
    WriteBit(cam, g_active, on);
    return true;
}

bool WriteLight(void* cam, bool on) {
    if (!cam || !EnsureResolved() || !IsCameraClass(cam)) return false;
    WriteBit(cam, g_light, on);
    return true;
}

bool CallUpd(void* cam) { return cam && component_calls::CallParamlessNamed(cam, L"upd"); }

bool CallUpdState(void* cam) { return cam && component_calls::CallParamlessNamed(cam, L"updState"); }

void* LaptopCamOf(void* widget) {
    if (!widget) return nullptr;
    const int32_t off = g_widgetCam.Of(widget);
    if (off < 0) return nullptr;
    void* cam = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(widget) + off);
    return (cam && R::IsLive(cam)) ? cam : nullptr;
}

bool CallUpdCam(void* widget) { return widget && component_calls::CallParamlessNamed(widget, L"updCam"); }

int RefreshLaptopsShowing(void* cam) {
    if (!cam) return 0;
    void* cls = object_index::ClassByName(kWidgetClass);
    if (!cls) return 0;
    // The widgets are collected first and called after: the walk's callback must not create objects, and a
    // repaint is a UFunction.
    struct Found { void* widgets[4]; int n = 0; void* cam; } found{};
    found.cam = cam;
    object_index::ForEachInstance(cls,
        [](void* ctx, void* widget, int32_t) {
            auto* f = static_cast<Found*>(ctx);
            if (f->n < 4 && LaptopCamOf(widget) == f->cam) f->widgets[f->n++] = widget;
        },
        &found);
    int repainted = 0;
    for (int i = 0; i < found.n; ++i)
        if (CallUpdCam(found.widgets[i])) ++repainted;
    return repainted;
}

void* LaptopWidget() {
    void* cls = object_index::ClassByName(kWidgetClass);
    if (!cls) return nullptr;
    void* found = nullptr;
    object_index::ForEachInstance(cls,
        [](void* ctx, void* widget, int32_t) {
            auto* out = static_cast<void**>(ctx);
            if (!*out && !R::NameStartsWith(R::NameOf(widget), L"Default__")) *out = widget;
        },
        &found);
    return found;
}

bool SetLaptopCam(void* widget, void* cam) {
    if (!widget) return false;
    const int32_t off = g_widgetCam.Of(widget);
    if (off < 0) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(widget) + off) = cam;
    return true;
}

bool CallLaptopLightClick(void* widget) {
    return widget && component_calls::CallParamlessNamed(
        widget, L"BndEvt__umg_laptop_Button_camFl_K2Node_ComponentBoundEvent_31_OnButtonClickedEvent__DelegateSignature");
}

}  // namespace ue_wrap::security_camera
