// ue_wrap/devices/oven_screen.cpp -- see ue_wrap/devices/oven_screen.h.

#include "ue_wrap/devices/oven_screen.h"

#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"  // IsOven

namespace ue_wrap::oven_screen {
namespace {

namespace R = ue_wrap::reflection;

// kitchen_C's `widget`, and the screen's bools in the mask's bit order, each on the class it is read
// from.
R::InstanceOffset g_widget{L"widget"};
// UI_oven_C's back pointer to the kitchen_C it repairs.
R::InstanceOffset g_kitchen{L"kitchen"};
// UWidget::Visibility, the byte the screen's enter() sets visible and its quit() collapses.
R::InstanceOffset g_visibility{L"Visibility"};
R::InstanceOffset g_bools[5] = {R::InstanceOffset{L"TL_unscrewed"}, R::InstanceOffset{L"TR_unscrewed"},
                                R::InstanceOffset{L"BL_unscrewed"}, R::InstanceOffset{L"BR_unscrewed"},
                                R::InstanceOffset{L"lid_taken"}};

// The oven's screen and its five bools' addresses. Null when there is no live screen or a member
// does not resolve.
void* Screen(void* oven, bool* out[5]) {
    if (!ue_wrap::appliance::IsOven(oven)) return nullptr;
    const int32_t wOff = g_widget.Of(oven);
    if (wOff < 0) return nullptr;
    void* screen = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(oven) + wOff);
    if (!screen || !R::IsLive(screen)) return nullptr;
    for (int i = 0; i < 5; ++i) {
        const int32_t off = g_bools[i].Of(screen);
        if (off < 0) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                UE_LOGW("oven_screen: bool #%d does not resolve on the oven's screen -- its repair steps are not "
                        "read (log-once)", i);
            }
            return nullptr;
        }
        out[i] = reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(screen) + off);
    }
    return screen;
}

}  // namespace

void* OvenOfScreen(void* screen) {
    if (!screen || !R::IsLive(screen)) return nullptr;
    const int32_t off = g_kitchen.Of(screen);
    if (off < 0) return nullptr;
    void* oven = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(screen) + off);
    return oven && R::IsLive(oven) && ue_wrap::appliance::IsOven(oven) ? oven : nullptr;
}

void* OpenScreenOf(void* oven) {
    if (!ue_wrap::appliance::IsOven(oven)) return nullptr;
    const int32_t wOff = g_widget.Of(oven);
    if (wOff < 0) return nullptr;
    void* screen = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(oven) + wOff);
    if (!screen || !R::IsLive(screen)) return nullptr;
    const int32_t vOff = g_visibility.Of(screen);
    if (vOff < 0) return nullptr;
    // ESlateVisibility: 1 is Collapsed, which the screen's own quit() sets and its class default holds until enter().
    constexpr uint8_t kCollapsed = 1;
    return *(reinterpret_cast<const uint8_t*>(screen) + vOff) == kCollapsed ? nullptr : screen;
}

bool CallScreenQuit(void* screen) {
    return screen && R::IsLive(screen) && component_calls::CallParamlessNamed(screen, L"quit");
}

bool TryReadProgress(void* oven, uint8_t& mask) {
    bool* b[5] = {};
    if (!Screen(oven, b)) return false;
    mask = 0;
    for (int i = 0; i < 5; ++i)
        if (*b[i]) mask |= static_cast<uint8_t>(1u << i);
    return true;
}

bool RaiseProgress(void* oven, uint8_t mask) {
    bool* b[5] = {};
    void* screen = Screen(oven, b);
    if (!screen) return false;
    for (int i = 0; i < 5; ++i)
        if (mask & (1u << i)) *b[i] = true;
    return component_calls::CallParamlessNamed(screen, L"upd");
}

}  // namespace ue_wrap::oven_screen
