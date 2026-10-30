// coop/save/save_button_disable.cpp -- see coop/save/save_button_disable.h.

#include "coop/save/save_button_disable.h"

#include "coop/net/session.h"
#include "ui/multiplayer_menu.h"  // PauseMenu
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::save_button_disable {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace prof = ue_wrap::profile;
using ue_wrap::Call;
using ue_wrap::ParamFrame;

UE_FACT_FN(kRowEscape, "save_button_disable", L"mainPlayer_C", L"InpActEvt_Escape_K2Node_InputKeyEvent_0",
           Declared);
// ui_menu_C::Tick(FGeometry, float): ProcessEvent-dispatched while the menu is visible.
UE_FACT_FN(kRowMenuTick, "save_button_disable", L"ui_menu_C", L"Tick", Declared);

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};

// Resolved ONCE at install (UClasses + UFunctions never move). Reused per-apply.
void* g_setEnabledFn = nullptr;   // UWidget::SetIsEnabled(bool bInIsEnabled)
void* g_setOpacityFn = nullptr;   // UWidget::SetRenderOpacity(float InOpacity)
void* g_getEnabledFn = nullptr;   // UWidget::GetIsEnabled()->bool (diagnostic read-back)
void* g_escFn = nullptr;          // mainPlayer_C::InpActEvt_Escape (per-open anchor)
void* g_tickFn = nullptr;         // ui_menu_C::Tick (self-heal anchor)
int32_t g_buttonSaveOff = -1;     // ui_menu_C -> button_Save (UButton*)

// UWidget::bIsEnabled is a PACKED bitfield @ +0xB4 (bit 0x04; siblings bIsVariable=0x01,
// bCreatedByConstructionScript=0x02, bOverride_Cursor=0x08). READING a bit is safe; WRITING
// the byte raw would corrupt the siblings + skip Slate invalidation -- so we WRITE only via
// the SetIsEnabled UFunction and READ the bit directly for the cheap per-tick self-heal check.
constexpr size_t kUWidgetIsEnabledByteOff = 0xB4;
constexpr uint8_t kUWidgetIsEnabledMask = 0x04;

coop::net::Session* Sess() { return g_session.load(std::memory_order_acquire); }
bool IsClient() {
    auto* s = Sess();
    return s && s->role() == coop::net::Role::Client;
}

bool ButtonEnabledRaw(void* button) {
    const auto byte = *(reinterpret_cast<const uint8_t*>(button) + kUWidgetIsEnabledByteOff);
    return (byte & kUWidgetIsEnabledMask) != 0;
}

// Disable + visually grey button_Save. Idempotent (writing the same enabled/opacity is a
// no-op). Game thread only (callers are ProcessEvent observers).
void ApplyGreyOut(void* button) {
    if (!button || !R::IsLive(button)) return;
    { ParamFrame f(g_setEnabledFn); f.Set<bool>(L"bInIsEnabled", false); Call(button, f); }
    { ParamFrame f(g_setOpacityFn); f.Set<float>(L"InOpacity", 0.35f); Call(button, f); }
    // Throttled read-back diagnostic, proving the disable took. First few applies only.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 && g_getEnabledFn) {
        ParamFrame f(g_getEnabledFn);
        const bool en = Call(button, f) ? f.Get<bool>(L"ReturnValue") : true;
        UE_LOGI("save_button_disable: greyed button_Save %p (apply #%llu, GetIsEnabled now=%d, opacity=0.35)",
                button, static_cast<unsigned long long>(n), en ? 1 : 0);
    }
}

// Return button_Save off a ui_menu instance IFF it is the pause variant (isPause==true).
// Null for the main-menu instance (shares ui_menu_C) or unresolved offsets.
void* PauseSaveButton(void* menu) {
    if (!menu || g_buttonSaveOff < 0) return nullptr;
    if (!coop::multiplayer_menu::IsPauseInstance(menu)) return nullptr;  // main menu -> leave alone
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(menu) + g_buttonSaveOff);
}

// POST observer on mainPlayer_C::InpActEvt_Escape: fires on each ESC press (engine input
// dispatch). Read the pause menu off the gamemode's own reference + disable its Save button. Null
// (no gamemode yet, the menu level) leaves nothing to grey; the next ESC asks again.
void OnEscapePost(void* /*self*/, void* /*function*/, void* /*params*/) {
    if (!IsClient()) return;
    if (void* btn = PauseSaveButton(coop::multiplayer_menu::PauseMenu())) ApplyGreyOut(btn);
}

// POST observer on ui_menu_C::Tick: `self` IS the menu (zero scan). Self-heal -- only
// re-applies when the BP has flipped the button back to enabled (cheap raw bit read; the
// UFunction write happens only on the re-enable edge, never per-frame).
void OnMenuTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!IsClient()) return;
    void* btn = PauseSaveButton(self);
    if (!btn || !R::IsLive(btn)) return;
    if (ButtonEnabledRaw(btn)) ApplyGreyOut(btn);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!session) return;
    g_session.store(session, std::memory_order_release);

    // Host: never disable its own Save button (the host keeps the canonical save). Latch.
    if (session->role() != coop::net::Role::Client) {
        g_installed.store(true, std::memory_order_release);
        return;
    }

    void* widgetCls = R::FindClass(prof::name::WidgetClass);
    void* mainPlayerCls = R::FindClass(prof::name::MainPlayerClass);
    void* uiMenuCls = R::FindClass(prof::name::UiMenuClass);
    if (!widgetCls || !mainPlayerCls || !uiMenuCls) return;  // BP not loaded yet -- retry next pump

    g_setEnabledFn = R::FindFunction(widgetCls, prof::name::WidgetSetIsEnabledFn);
    g_setOpacityFn = R::FindFunction(widgetCls, prof::name::WidgetSetRenderOpacityFn);
    g_getEnabledFn = R::FindFunction(widgetCls, prof::name::WidgetGetIsEnabledFn);
    const ue_wrap::facts::Resolved escRow = ue_wrap::facts::Resolve(kRowEscape);
    const ue_wrap::facts::Resolved tickRow = ue_wrap::facts::Resolve(kRowMenuTick);
    g_escFn = escRow.Fn();
    g_tickFn = tickRow.Fn();
    g_buttonSaveOff = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuButtonSaveProp);
    if (!g_setEnabledFn || !g_setOpacityFn || !g_escFn || !g_tickFn || g_buttonSaveOff < 0) {
        UE_LOGW("save_button_disable: resolve incomplete (setEnabled=%p setOpacity=%p esc=%p "
                "tick=%p btnOff=%d) -- retry next pump",
                g_setEnabledFn, g_setOpacityFn, g_escFn, g_tickFn, g_buttonSaveOff);
        return;
    }

    const bool okEsc = GT::RegisterPostObserver(escRow, &OnEscapePost);
    const bool okTick = GT::RegisterPostObserver(tickRow, &OnMenuTickPost);
    if (!okEsc || !okTick) {
        UE_LOGE("save_button_disable: RegisterPostObserver failed (esc=%d tick=%d) -- table full?",
                okEsc ? 1 : 0, okTick ? 1 : 0);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("save_button_disable: client pause-menu Save-button grey-out INSTALLED "
            "(escFn=%p tickFn=%p button_Save@+0x%X)",
            g_escFn, g_tickFn, static_cast<unsigned>(g_buttonSaveOff));
}

}  // namespace coop::save_button_disable
