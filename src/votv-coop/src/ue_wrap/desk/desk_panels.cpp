// ue_wrap/desk/desk_panels.cpp -- see ue_wrap/desk/desk_panels.h.

#include "ue_wrap/desk/desk_panels.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <chrono>

namespace ue_wrap::desk_panels {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

// Class-level, as ue_wrap/desk/phys_mods keeps its own: the desk class is the same object in every
// world, and the panel prop is the desk's own import.
void*   g_deskCls = nullptr;
int32_t g_offState = -1;     // panelsState, TArray<bool>
int32_t g_offUnscrew = -1;   // panelsUnscrew, TArray<bool>
int32_t g_offTakeOff = -1;   // panelTakeOff, bool
void*   g_updPanelsFn = nullptr;
void*   g_panelBaseCls = nullptr;  // Aprop_computerBackPanel_C
// The desk's take-off picks the prop class by the panel's index: top left, bottom left, top right,
// bottom right.
constexpr const wchar_t* kPanelClassNames[kPanels] = {
    L"prop_computerBackPanel_TL_C", L"prop_computerBackPanel_BL_C", L"prop_computerBackPanel_TR_C",
    L"prop_computerBackPanel_BR_C"};
void*   g_panelCls[kPanels] = {};
bool    g_resolved = false;
Clock::time_point g_nextTry{};

// UE TArray layout: data ptr @0, Num @8. A bool element is one byte. Null when the array is shorter
// than kPanels.
uint8_t* Elements(void* desk, int32_t off) {
    auto* base = reinterpret_cast<uint8_t*>(desk) + off;
    uint8_t* data = *reinterpret_cast<uint8_t**>(base);
    const int32_t num = *reinterpret_cast<int32_t*>(base + 8);
    return (data && num >= kPanels) ? data : nullptr;
}

void* ResolvedDesk() {
    return g_resolved ? ue_wrap::console_desk::Instance() : nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const auto now = Clock::now();
    if (now < g_nextTry) return false;
    g_nextTry = now + std::chrono::seconds(1);

    if (!ue_wrap::console_desk::EnsureResolved()) return false;
    if (!g_deskCls) g_deskCls = R::FindClass(L"analogDScreenTest_C");
    if (!g_deskCls) return false;
    if (g_offState < 0) g_offState = R::FindPropertyOffset(g_deskCls, L"panelsState");
    if (g_offUnscrew < 0) g_offUnscrew = R::FindPropertyOffset(g_deskCls, L"panelsUnscrew");
    if (g_offTakeOff < 0) g_offTakeOff = R::FindPropertyOffset(g_deskCls, L"panelTakeOff");
    if (!g_updPanelsFn) g_updPanelsFn = R::FindFunction(g_deskCls, L"updPanels");
    if (!g_panelBaseCls) g_panelBaseCls = R::FindClass(L"prop_computerBackPanel_C");
    bool panelClasses = true;
    for (int i = 0; i < kPanels; ++i) {
        if (!g_panelCls[i]) g_panelCls[i] = R::FindClass(kPanelClassNames[i]);
        panelClasses = panelClasses && g_panelCls[i];
    }
    if (g_offState < 0 || g_offUnscrew < 0 || g_offTakeOff < 0 || !g_updPanelsFn || !g_panelBaseCls ||
        !panelClasses) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("desk_panels: resolve incomplete (state=%d unscrew=%d takeOff=%d upd=%d panelClass=%d "
                    "placeClasses=%d) -- backoff retry (log-once)", g_offState >= 0 ? 1 : 0,
                    g_offUnscrew >= 0 ? 1 : 0, g_offTakeOff >= 0 ? 1 : 0, g_updPanelsFn ? 1 : 0,
                    g_panelBaseCls ? 1 : 0, panelClasses ? 1 : 0);
        }
        return false;
    }
    g_resolved = true;
    UE_LOGI("desk_panels: resolved (panelsState@0x%X panelsUnscrew@0x%X panelTakeOff@0x%X + updPanels + "
            "prop_computerBackPanel_C)", g_offState, g_offUnscrew, g_offTakeOff);
    return true;
}

bool Read(Masks& out) {
    void* d = ResolvedDesk();
    if (!d) return false;
    const uint8_t* state = Elements(d, g_offState);
    const uint8_t* unscrew = Elements(d, g_offUnscrew);
    if (!state || !unscrew) return false;
    out = Masks{};
    for (int i = 0; i < kPanels; ++i) {
        if (state[i]) out.on |= static_cast<uint8_t>(1u << i);
        if (unscrew[i]) out.unscrewed |= static_cast<uint8_t>(1u << i);
    }
    return true;
}

bool Write(const Masks& in) {
    void* d = ResolvedDesk();
    if (!d) return false;
    uint8_t* state = Elements(d, g_offState);
    uint8_t* unscrew = Elements(d, g_offUnscrew);
    if (!state || !unscrew) {
        UE_LOGW("desk_panels: Write refused (an array is shorter than %d panels)", kPanels);
        return false;
    }
    for (int i = 0; i < kPanels; ++i) {
        state[i] = (in.on >> i) & 1u;
        unscrew[i] = (in.unscrewed >> i) & 1u;
    }
    return true;
}

bool WriteTakeOff(bool on) {
    void* d = ResolvedDesk();
    if (!d) return false;
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(d) + g_offTakeOff) = on;
    return true;
}

bool CallUpdPanels() {
    void* d = ResolvedDesk();
    if (!d) return false;
    ue_wrap::ParamFrame f(g_updPanelsFn);
    return f.valid() && ue_wrap::Call(d, f);
}

bool IsPanelClass(void* cls) {
    if (!cls || !g_panelBaseCls) return false;
    return cls == g_panelBaseCls || R::IsDescendantOfAny(cls, &g_panelBaseCls, 1);
}

void* ClassForPanel(int panel) {
    return (g_resolved && panel >= 0 && panel < kPanels) ? g_panelCls[panel] : nullptr;
}

int PanelForClass(void* cls) {
    if (!cls || !g_resolved) return -1;
    for (int i = 0; i < kPanels; ++i)
        if (cls == g_panelCls[i]) return i;
    return -1;
}

}  // namespace ue_wrap::desk_panels
