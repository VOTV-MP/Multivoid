// ue_wrap/devices/laptop_widget.cpp -- see ue_wrap/devices/laptop_widget.h.

#include "ue_wrap/devices/laptop_widget.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/umg_build.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::laptop_widget {
namespace {

namespace R = reflection;

R::InstanceOffset g_gamemodeLaptop{L"laptop"};   // mainGamemode_C::laptop, the widget
R::InstanceOffset g_widgetSwitcher{L"switcher"};  // ui_laptop_C::switcher, a UWidgetSwitcher

// The live object in `obj`'s pointer member at `off`, or null.
void* LiveAt(void* obj, int32_t off) {
    if (!obj || off < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

void* SwitcherOf(void* widget) {
    return widget ? LiveAt(widget, g_widgetSwitcher.Of(widget)) : nullptr;
}

}  // namespace

void* Widget() {
    void* gm = world_singleton::Gamemode();
    return gm ? LiveAt(gm, g_gamemodeLaptop.Of(gm)) : nullptr;
}

bool ReadTab(void* widget, int& index) {
    void* sw = SwitcherOf(widget);
    if (!sw) return false;
    const int32_t i = umg::SwitcherIndex(sw);
    if (i < 0) return false;
    index = i;
    return true;
}

bool TabCount(void* widget, int& n) {
    void* sw = SwitcherOf(widget);
    if (!sw) return false;
    const int32_t c = umg::ChildCount(sw);
    if (c < 0) return false;
    n = c;
    return true;
}

bool WriteTab(void* widget, int index) {
    void* sw = SwitcherOf(widget);
    return sw && index >= 0 && umg::SwitcherSetIndex(sw, index);
}

}  // namespace ue_wrap::laptop_widget
