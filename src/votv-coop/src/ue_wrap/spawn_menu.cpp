// ue_wrap/spawn_menu.cpp -- see header.

#include "ue_wrap/spawn_menu.h"

#include "coop/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"      // GetController -- the local player's PlayerController for input mode
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::spawn_menu {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// Cached refs for the open-state diagnostic (the open itself runs via ExecuteUbergraph; see Open()).
void*   g_spawnMenuCls = nullptr;  // ui_spawnmenu_C
int32_t g_visOff       = -2;       // UWidget.Visibility byte offset (-2 unresolved, -1 none)

// The live (non-CDO) ui_spawnmenu_C widget instance. propProcessor creates ONE per world at
// startup (`ExecuteUbergraph_propProcessor: WidgetBlueprintLibrary.Create(ui_spawnmenu_C) +
// AddToViewport`) in EVERY gamemode -- verified live: in story it exists at Visibility=1
// (Collapsed). nullptr only if the world hasn't created it yet.
void* FindSpawnMenuWidget() {
    if (!g_spawnMenuCls) g_spawnMenuCls = R::FindClass(L"ui_spawnmenu_C");
    if (!g_spawnMenuCls) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || R::ClassOf(obj) != g_spawnMenuCls) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (!R::IsLive(obj)) continue;
        return obj;
    }
    return nullptr;
}

// ESlateVisibility byte of `widget` (0=Visible, 1=Collapsed, 2=Hidden, ...), or -3 if the
// offset is unresolved. Used as the before/after open diagnostic.
int ReadVis(void* widget) {
    if (!widget || !g_spawnMenuCls) return -3;
    if (g_visOff == -2) g_visOff = R::FindPropertyOffset(g_spawnMenuCls, L"Visibility");
    if (g_visOff < 0) return -3;
    return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(widget) + g_visOff);
}

}  // namespace

bool Open() {
    void* localPlayer = coop::players::Registry::Get().Local();
    if (!localPlayer) {
        UE_LOGW("spawn_menu::Open: no local mainPlayer_C (not in a world yet) -- not opened");
        return false;
    }

    // The vanilla open guard: the BP open ubergraph (@12104) NO-OPs if `activeInterface` is valid
    // (another UI is up). Honour it so we never stack the spawn menu over another menu. Reflection
    // offset cached; a miss just skips the guard.
    {
        static int32_t sActiveIfaceOff = -2;
        if (sActiveIfaceOff == -2) {
            void* cls = R::FindClass(P::name::MainPlayerClass);
            sActiveIfaceOff = cls ? R::FindPropertyOffset(cls, L"activeInterface") : -1;
        }
        if (sActiveIfaceOff >= 0) {
            void* iface = *reinterpret_cast<void* const*>(
                reinterpret_cast<const uint8_t*>(localPlayer) + sActiveIfaceOff);
            if (iface) {
                UE_LOGW("spawn_menu::Open: mainPlayer.activeInterface is SET (%p) -- another UI is "
                        "open; not opening the spawn menu over it (close it first)", iface);
                return false;
            }
        }
    }

    // ROOT CAUSE (2026-06-15, verified live): calling the native input-event UFunction
    // (InpActEvt_spawnmenu_..._2) directly is a NO-OP -- the engine's input system wires the
    // forward to the open ubergraph, not the stub body, so the dispatch left the widget Collapsed
    // (Visibility 1 -> 1). And UWidget::SetVisibility is native (not reachable via FindFunction on
    // the BP class). The reliable fix is the engine's OWN dispatch mechanism: a BP event IS a call
    // to ExecuteUbergraph_<BP>(EntryPoint), so invoke ExecuteUbergraph_mainPlayer at the spawn-menu
    // OPEN entry (@12077, from the kismet -- the exact target the InpActEvt event forwards to). That
    // runs the FULL native open block: the activeInterface/isBuoyant guards, SetVisibility(Visible),
    // SetInputMode_GameAndUIEx (cursor) and opened() (content) -- exactly as a real Q press, no asset
    // edit (RULE 3). The block copies the (unused-for-gating) input Key from a zeroed frame, so the
    // empty FKey is fine.
    void* widget = FindSpawnMenuWidget();
    if (!widget) {
        UE_LOGW("spawn_menu::Open: no live ui_spawnmenu_C widget in this world yet -- cannot open");
        return false;
    }
    const int visBefore = ReadVis(widget);  // resolves g_visOff
    // Call the NATIVE UWidget::SetVisibility (resolved on the native "Widget" UClass -- FindFunction
    // on the BP class doesn't reach inherited native UFunctions) so the change propagates to the live
    // Slate widget (a raw byte write to Visibility leaves the cached SWidget collapsed -- proven: the
    // byte went 1->0 but the menu still didn't render). Then run the widget's own opened() content
    // setup. The input-stub / ExecuteUbergraph-by-kismet-offset routes are no-ops -- see the doc.
    static void* sSetVisFn = nullptr;
    if (!sSetVisFn) {
        void* widgetCls = R::FindClass(L"Widget");  // UWidget
        sSetVisFn = widgetCls ? R::FindFunction(widgetCls, L"SetVisibility") : nullptr;
    }
    if (sSetVisFn) {
        ue_wrap::ParamFrame f(sSetVisFn);
        if (f.valid()) {
            f.Set<uint8_t>(L"InVisibility", 0);  // ESlateVisibility::Visible
            ue_wrap::Call(widget, f);
        }
    } else if (g_visOff >= 0) {
        *(reinterpret_cast<uint8_t*>(widget) + g_visOff) = 0;  // fallback: raw byte (may not render)
        UE_LOGW("spawn_menu::Open: native UWidget::SetVisibility unresolved -- raw byte fallback (Slate may not render)");
    }
    static void* sOpenedFn = nullptr;
    if (!sOpenedFn) sOpenedFn = R::FindFunction(g_spawnMenuCls, L"opened");
    if (sOpenedFn) {
        ue_wrap::ParamFrame f(sOpenedFn);
        if (f.valid()) ue_wrap::Call(widget, f);
    }

    // Cursor + UI input so the rendered menu is CLICKABLE (the native open path's
    // SetInputMode_GameAndUIEx(PC, spawnmenu, ...) @12494 + show the mouse cursor). Without this the
    // menu renders but the game stays GameOnly -> no cursor, can't click a prop to spawn it.
    if (void* pc = ue_wrap::engine::GetController(localPlayer)) {
        static void* sWblCdo = nullptr;
        static void* sSetInputFn = nullptr;
        if (!sWblCdo) sWblCdo = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
        if (sWblCdo && !sSetInputFn) sSetInputFn = R::FindFunction(R::ClassOf(sWblCdo), L"SetInputMode_GameAndUIEx");
        if (sWblCdo && sSetInputFn) {
            ue_wrap::ParamFrame f(sSetInputFn);
            if (f.valid()) {
                f.Set<void*>(L"PlayerController", pc);
                f.Set<void*>(L"InWidgetToFocus", widget);
                f.Set<uint8_t>(L"InMouseLockMode", 0);            // EMouseLockMode::DoNotLock
                f.Set<uint8_t>(L"bHideCursorDuringCapture", 0);   // keep the cursor
                ue_wrap::Call(sWblCdo, f);
            }
        }
        static int32_t sCursorOff = -2;
        if (sCursorOff == -2) sCursorOff = R::FindPropertyOffset(R::ClassOf(pc), L"bShowMouseCursor");
        if (sCursorOff >= 0) *(reinterpret_cast<uint8_t*>(pc) + sCursorOff) = 1;
    }

    const int visAfter = ReadVis(widget);
    UE_LOGI("spawn_menu::Open: native SetVisibility(Visible) + opened() + GameAndUI input -- "
            "visibility %d -> %d (0=Visible)", visBefore, visAfter);
    return true;
}

}  // namespace ue_wrap::spawn_menu
