// coop/dev/native_ui_probe.cpp -- see coop/dev/native_ui_probe.h.

#include "coop/dev/native_ui_probe.h"

#include "coop/config/config.h"
#include "coop/dev/dev_gate.h"
#include "coop/input/input_owner.h"
#include "harness/screenshot.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::native_ui_probe {
namespace {

namespace R   = ue_wrap::reflection;
namespace GT  = ue_wrap::game_thread;
namespace E   = ue_wrap::engine;
namespace P   = ue_wrap::profile;
namespace WID = ue_wrap::world_identity;

bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::native_ui_probe);
    return s;
}
bool WriteArmed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::native_ui_probe_write);
    return s && Armed();
}

// =====================================================================================
// Small helpers (this TU only).
// =====================================================================================

inline void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// Resolve one UFunction on the class that OWNS it and log the verdict. R::FindFunction
// matches `OuterOf(fn) == owningClass` with NO super-walk (reflection.cpp:493), so the
// owning class IS the question here -- this is O1.
void* ResolveFnOn(const wchar_t* className, const wchar_t* fnName, int* okCount, int* missCount) {
    void* cls = R::FindClass(className);
    if (!cls) {
        UE_LOGW("[native_ui_probe] O1 class MISS: %ls (so %ls is unresolvable)", className, fnName);
        if (missCount) ++*missCount;
        return nullptr;
    }
    void* fn = R::FindFunction(cls, fnName);
    if (fn) {
        UE_LOGI("[native_ui_probe] O1 ok   %ls::%ls -> %p", className, fnName, fn);
        if (okCount) ++*okCount;
    } else {
        UE_LOGW("[native_ui_probe] O1 MISS %ls::%ls (class %p resolved, function did not -- wrong "
                "OWNING class, or renamed by a recook)", className, fnName, cls);
        if (missCount) ++*missCount;
    }
    return fn;
}

// =====================================================================================
// STAGE A -- the read-only census. Runs once per menu instance.
// =====================================================================================

// Every class the browser instantiates and every UFunction it calls, paired with the
// class that OWNS the function. Kept as data rather than code so a recook diff is a
// table diff.
struct FnRow { const wchar_t* cls; const wchar_t* fn; };
constexpr FnRow kFnTable[] = {
    // The panel API -- ONE resolve on UPanelWidget serves ScrollBox, Overlay,
    // HorizontalBox, CanvasPanel and the WidgetSwitcher alike. `AddChild` is resolved
    // NOWHERE in the tree today, and UWidgetSwitcher has no typed AddChildToX, so this
    // row is the most load-bearing line in the table.
    {L"PanelWidget",        L"AddChild"},
    {L"PanelWidget",        L"RemoveChild"},
    {L"PanelWidget",        L"GetChildrenCount"},
    {L"PanelWidget",        L"GetChildAt"},
    {L"PanelWidget",        L"GetChildIndex"},
    // The switcher itself (placement).
    {L"WidgetSwitcher",     L"SetActiveWidgetIndex"},
    {L"WidgetSwitcher",     L"GetActiveWidgetIndex"},
    {L"WidgetSwitcher",     L"GetNumWidgets"},
    // Typed adds (each panel type has its own; cheaper than AddChild where it exists).
    {L"Overlay",            L"AddChildToOverlay"},
    {L"HorizontalBox",      L"AddChildToHorizontalBox"},
    {L"CanvasPanel",        L"AddChildToCanvas"},
    // The row's own widgets.
    {L"Image",              L"SetBrushFromMaterial"},
    {L"Image",              L"SetBrushFromTexture"},
    {L"Image",              L"SetBrushTintColor"},
    {L"Image",              L"SetBrushSize"},
    {L"SizeBox",            L"SetHeightOverride"},
    {L"SizeBox",            L"SetWidthOverride"},
    // Chrome.
    {L"EditableTextBox",    L"SetText"},
    {L"EditableTextBox",    L"GetText"},
    {L"ScrollBox",          L"SetScrollOffset"},
    {L"ScrollBox",          L"GetScrollOffset"},
    {L"ScrollBox",          L"ScrollToEnd"},
    // Slot setters (alignment-driven layout, no offset math).
    {L"OverlaySlot",        L"SetHorizontalAlignment"},
    {L"OverlaySlot",        L"SetVerticalAlignment"},
    {L"OverlaySlot",        L"SetPadding"},
    {L"HorizontalBoxSlot",  L"SetSize"},
    {L"HorizontalBoxSlot",  L"SetPadding"},
    {L"HorizontalBoxSlot",  L"SetHorizontalAlignment"},
    {L"WidgetSwitcherSlot", L"SetHorizontalAlignment"},
    // UWidget-owned reads the hover/geometry path needs. GetDesiredSize and
    // GetCachedGeometry are also RUNG 1's non-visual instruments.
    {L"Widget",             L"IsHovered"},
    {L"Widget",             L"GetDesiredSize"},
    {L"Widget",             L"GetCachedGeometry"},
    {L"Widget",             L"SetVisibility"},
};

// Classes we only need to INSTANTIATE (no function of their own to resolve).
constexpr const wchar_t* kClassOnly[] = {
    L"ScrollBox", L"Overlay", L"SizeBox", L"Image", L"HorizontalBox", L"EditableTextBox",
    L"CanvasPanel", L"WidgetSwitcher", L"UserWidget", L"WidgetTree", L"TextBlock",
};

// The style donors section 8's table names, with the class each is expected on. The
// point is not that they resolve -- it is whether they are RESIDENT and NON-NULL at
// MAIN-MENU time, because the styling rule is FAIL-CLOSED: a null donor must mean
// "retry", never "fall back to a default style" (that fallback is the Roboto / centred /
// white bug).
struct DonorRow { const wchar_t* cls; const wchar_t* field; const char* role; };
constexpr DonorRow kDonorTable[] = {
    {L"ui_menu_C",      L"button_start",   "button, 3 states + both sounds -- the shipped inject's own donor"},
    {L"ui_menu_C",      L"Image_0",        "panel fill (menu-resident)"},
    {L"ui_menu_C",      L"Image_61",       "panel fill / border candidate (menu-resident)"},
    {L"ui_saveSlots_C", L"Image_0",        "panel fill"},
    {L"ui_saveSlots_C", L"Image_6",        "border candidate"},
    {L"ui_saveSlots_C", L"button_back",    "button"},
    {L"ui_saveSlots_C", L"ETB_slotName",   "text box"},
    {L"ui_saveSlots_C", L"ScrollBox_list", "scroll container (unstyled -- ui_settings owns the bar style)"},
    {L"ui_settings_C",  L"scrollboxRoot",  "scrollbar (WidgetBarStyle / inst_uiScroll)"},
    {L"ui_settings_C",  L"rtb_desc",       "description pane"},
    // Named by the section-8 donor table but NOT a field on ui_saveSlots_C in this
    // build's header dump -- listed so the log says so out loud instead of the doc
    // continuing to name a donor nothing can read.
    {L"ui_saveSlots_C", L"image_border",   "section-8 'image_border_*' -- expected ABSENT, confirming the doc row is wrong"},
};

// FScriptDelegate: TWeakObjectPtr {int32 index, int32 serial} + FName {int32, int32}.
struct ScriptDelegate {
    int32_t objectIndex;
    int32_t objectSerial;
    R::FName functionName;
};
// A multicast delegate property is a TArray<FScriptDelegate> -- {ptr, num, max}.
struct MulticastDelegate {
    ScriptDelegate* data;
    int32_t num;
    int32_t max;
};

bool  g_stageADone  = false;
void* g_stageAMenu  = nullptr;

void*   g_switcher     = nullptr;  // ui_menu_C::switcher_widgets (re-read per tick)
int32_t g_switcherOff  = -1;
void*   g_fnAddChild   = nullptr, *g_fnRemoveChild = nullptr, *g_fnChildCount = nullptr;
void*   g_fnGetChildAt = nullptr;
void*   g_fnSetActiveIdx = nullptr, *g_fnGetActiveIdx = nullptr;
void*   g_fnDesiredSize  = nullptr, *g_fnIsHovered    = nullptr;

int32_t CallIntNoArg(void* obj, void* fn) {
    if (!obj || !fn) return -1;
    ue_wrap::ParamFrame f(fn);
    if (!Call(obj, f)) return -1;
    return f.Get<int32_t>(L"ReturnValue");
}

ue_wrap::FVector2D DesiredSizeOf(void* widget) {
    ue_wrap::FVector2D v{0.f, 0.f};
    if (!widget || !g_fnDesiredSize) return v;
    ue_wrap::ParamFrame f(g_fnDesiredSize);
    if (!Call(widget, f)) return v;
    f.GetRaw(L"ReturnValue", &v, sizeof(v));
    return v;
}

// O5 -- the FSlateResourceHandle question. FSlateBrush is 0x88: reflected fields end at
// ImageType @0x6F and the bitfield bools resume @0x80, so the 16 bytes at +0x70 are an
// unreflected FSlateResourceHandle (a TSharedPtr). InjectCanvasButton memcpys the whole
// 0x278 FButtonStyle, which spans four brushes at 0x08 / 0x90 / 0x118 / 0x1A0
// (SlateCore.hpp:12-15) -- so if this handle is populated we shallow-alias a refcounted
// pointer with no AddRef, exactly as the FSlateSound tail once did.
void MeasureBrushHandles(void* donorButton, const wchar_t* label) {
    if (!donorButton) {
        UE_LOGW("[native_ui_probe] O5 SKIPPED (%ls) -- no donor button", label);
        return;
    }
    static constexpr size_t      kBrushOff[4] = {0x08, 0x90, 0x118, 0x1A0};
    static constexpr const char* kName[4]     = {"Normal", "Hovered", "Pressed", "Disabled"};
    static constexpr size_t kHandleOff = 0x70, kResourceObjOff = 0x48;
    auto* style = reinterpret_cast<uint8_t*>(donorButton) + P::off::UButton_WidgetStyle;
    int populated = 0, withArt = 0;
    for (int i = 0; i < 4; ++i) {
        auto* h = style + kBrushOff[i] + kHandleOff;
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, h, 8);
        std::memcpy(&hi, h + 8, 8);
        const bool live = (lo != 0) || (hi != 0);
        if (live) ++populated;
        // ResourceObject is printed beside it: a brush with no resource at all cannot
        // have a handle, which would make a zero here uninformative rather than an answer.
        void* res = *reinterpret_cast<void**>(style + kBrushOff[i] + kResourceObjOff);
        if (res) ++withArt;
        UE_LOGI("[native_ui_probe] O5 %ls brush %-8s ResourceObject=%p  handle@+0x%zX = %016llX "
                "%016llX  -> %s",
                label, kName[i], res, kHandleOff, static_cast<unsigned long long>(lo),
                static_cast<unsigned long long>(hi), live ? "POPULATED" : "null");
    }
    // THE VERDICT MAY NOT BE STRONGER THAN THE EVIDENCE, and this probe's own first run
    // proved why the guard is needed: ui_menu_C.button_start's four brushes carry NO
    // ResourceObject at all, so all four handles read zero -- and a handle is a CACHE OF A
    // RESOURCE. Zero from a brush with no resource says nothing about whether a brush WITH
    // one caches a handle, which is the actual question. The block already carried that
    // caveat as a comment; the verdict now obeys it instead of contradicting it.
    if (populated > 0)
        UE_LOGW("[native_ui_probe] O5 VERDICT (%ls): %d/4 handles POPULATED -- P0 is ARMED (the "
                "0x278 memcpy shallow-aliases a refcounted TSharedPtr with no AddRef)",
                label, populated);
    else if (withArt == 0)
        UE_LOGW("[native_ui_probe] O5 INCONCLUSIVE (%ls): 0/4 handles set, but 0/4 brushes carry a "
                "ResourceObject either -- a handle CACHES a resource, so this donor cannot answer "
                "the question. Needs a donor that actually draws something.",
                label);
    else
        UE_LOGI("[native_ui_probe] O5 VERDICT (%ls): 0/4 handles populated across %d/4 brushes that "
                "DO carry a ResourceObject -- there is no handle bug, and P0 does not touch the one "
                "hands-on-verified inject",
                label, withArt);
}

// O8 -- read-only proof of the delegate layout. Nothing is written: reading the GAME's
// OWN bound button confirms UButton::OnClicked @ +0x3C8 (UMG.hpp:294) AND confirms there
// is a delegate -> ProcessEvent path to point at, which is the whole of the evidence the
// v2 "retire the poll" decision needs. v1 polls either way.
void MeasureDelegate(void* donorButton, const wchar_t* label) {
    if (!donorButton) return;
    static constexpr size_t kOnClickedOff = 0x3C8;
    auto* md = reinterpret_cast<MulticastDelegate*>(reinterpret_cast<uint8_t*>(donorButton) +
                                                    kOnClickedOff);
    if (md->num < 0 || md->num > 64 || (md->num > 0 && !md->data)) {
        UE_LOGW("[native_ui_probe] O8 %ls: OnClicked@+0x%zX looks WRONG (num=%d max=%d data=%p) -- "
                "offset stale for this build?",
                label, kOnClickedOff, md->num, md->max, static_cast<void*>(md->data));
        return;
    }
    UE_LOGI("[native_ui_probe] O8 %ls: OnClicked@+0x%zX num=%d max=%d", label, kOnClickedOff,
            md->num, md->max);
    for (int i = 0; i < md->num && i < 4; ++i) {
        const std::wstring fn = R::ToString(md->data[i].functionName);
        UE_LOGI("[native_ui_probe] O8   [%d] objIdx=%d serial=%d fn='%ls'", i,
                md->data[i].objectIndex, md->data[i].objectSerial, fn.c_str());
    }
}

// A5 -- the switcher CHILD MAP. Nobody has measured which sub-screen sits at which index,
// and P2's placement is stated in indices ("our screen is the 12th child, index 11"). It
// is also the live half of the placement's safety argument: appending cannot renumber
// what is already there.
// The live sub-screen instances, captured off the switcher's own child list so there is
// no ambiguity about WHICH instance was read. Both are also looked up by
// R::FindObjectByClass in the donor pass, and the two answers are printed side by side --
// because "the donor is null" and "I read a different object than the one on screen" have
// the same shape in a log and only one of them is a finding.
void* g_childSaveSlots = nullptr;
void* g_childSettings  = nullptr;
int32_t g_idxSaveSlots = -1;

void MeasureSwitcher(void* menu) {
    g_switcherOff = R::FindPropertyOffset(R::ClassOf(menu), L"switcher_widgets");
    g_switcher    = ReadPtr(menu, g_switcherOff);
    if (!g_switcher) {
        UE_LOGW("[native_ui_probe] A5: switcher_widgets unresolved (off=%d) -- placement UNMEASURED",
                g_switcherOff);
        return;
    }
    const int32_t n   = CallIntNoArg(g_switcher, g_fnChildCount);
    const int32_t act = CallIntNoArg(g_switcher, g_fnGetActiveIdx);
    UE_LOGI("[native_ui_probe] A5: switcher_widgets@+0x%X = %p (class %ls)  children=%d  "
            "activeIndex=%d",
            static_cast<unsigned>(g_switcherOff), g_switcher, R::ClassNameOf(g_switcher).c_str(), n,
            act);
    if (!g_fnGetChildAt) return;
    for (int32_t i = 0; i < n && i < 32; ++i) {
        ue_wrap::ParamFrame f(g_fnGetChildAt);
        f.Set<int32_t>(L"Index", i);
        if (!Call(g_switcher, f)) continue;
        void* child = f.Get<void*>(L"ReturnValue");
        const std::wstring cn = child ? R::ClassNameOf(child) : L"<null>";
        if (cn == L"ui_saveSlots_C") { g_childSaveSlots = child; g_idxSaveSlots = i; }
        if (cn == L"ui_settings_C")  { g_childSettings  = child; }
        UE_LOGI("[native_ui_probe] A5   [%2d] %p  %ls", i, child, cn.c_str());
    }
}

// O7 -- donor residency.
//
// The ui_menu_C rows read the LIVE menu we were handed. For a sub-screen class the owner
// is taken from the SWITCHER'S OWN CHILD LIST (captured in A5) and falls back to
// R::FindObjectByClass; both pointers are printed, because "the donor is null" and "I read
// a different instance than the one in the tree" look identical in a log and only one of
// them is a finding.
//
// THAT COMPARISON IS NOT DECORATION -- it caught a wrong conclusion on this probe's first
// run. Reading through FindObjectByClass alone reported EVERY widget field on
// ui_saveSlots_C and ui_settings_C as null, which read as "sub-screen donors do not exist
// until the screen is shown" and would have forced section 8 to redesign its donor table
// around a precondition. Measured with both pointers: FindObjectByClass returns a
// DIFFERENT non-CDO instance than the one in the switcher (a WidgetBlueprint carries a
// tree template that is not named `Default__`, so the CDO skip does not exclude it), and
// the live child's donors are all RESIDENT at menu time. `FindObjectByClass` answers
// "the first instance", which is not "the live one".
//
// The FindObjectByClass fallback is MEMOISED per class name, not per row: it walks all
// ~237k GUObjectArray entries and the table names ui_saveSlots_C six times.
void MeasureDonors(void* menu, const char* phase) {
    const wchar_t* lastCls = nullptr;
    void* lastFound = nullptr;
    for (const DonorRow& d : kDonorTable) {
        const std::wstring cls(d.cls);
        void* owner = nullptr;
        void* found = nullptr;
        if (cls == L"ui_menu_C") {
            owner = menu;
        } else {
            if (lastCls && std::wstring(lastCls) == cls) {
                found = lastFound;
            } else {
                found = R::FindObjectByClass(d.cls);
                lastCls = d.cls;
                lastFound = found;
            }
            void* child = (cls == L"ui_saveSlots_C")  ? g_childSaveSlots
                        : (cls == L"ui_settings_C")   ? g_childSettings
                                                      : nullptr;
            owner = child ? child : found;
            if (child && found && child != found)
                UE_LOGW("[native_ui_probe] O7 %ls: the switcher's child (%p) is NOT what "
                        "FindObjectByClass returns (%p) -- reading the CHILD", d.cls, child, found);
        }
        if (!owner) {
            UE_LOGW("[native_ui_probe] O7[%s] %ls.%ls -- OWNER ABSENT (%s)", phase, d.cls, d.field,
                    d.role);
            continue;
        }
        const int32_t off = R::FindPropertyOffset(R::ClassOf(owner), d.field);
        if (off < 0) {
            UE_LOGW("[native_ui_probe] O7[%s] %ls.%ls -- NO SUCH FIELD on the class (%s)", phase,
                    d.cls, d.field, d.role);
            continue;
        }
        void* w = ReadPtr(owner, off);
        UE_LOGI("[native_ui_probe] O7[%s] %ls.%ls @+0x%X owner=%p -> %p  %s  (%s)", phase, d.cls,
                d.field, static_cast<unsigned>(off), owner, w,
                w ? "RESIDENT" : "NULL -- fail-closed retry", d.role);
    }
}

void RunStageA(void* menu) {
    UE_LOGI("[native_ui_probe] ===== STAGE A on menu=%p (class %ls) =====", menu,
            R::ClassNameOf(menu).c_str());

    // O1 -- the resolve census.
    int ok = 0, miss = 0;
    for (const wchar_t* c : kClassOnly) {
        void* cls = R::FindClass(c);
        if (cls) { UE_LOGI("[native_ui_probe] O1 ok   class %ls -> %p", c, cls); ++ok; }
        else     { UE_LOGW("[native_ui_probe] O1 MISS class %ls", c);            ++miss; }
    }
    for (const FnRow& r : kFnTable) ResolveFnOn(r.cls, r.fn, &ok, &miss);
    UE_LOGI("[native_ui_probe] O1 SUMMARY: %d resolved, %d MISSING (of %d)", ok, miss,
            static_cast<int>(sizeof(kClassOnly) / sizeof(kClassOnly[0]) +
                             sizeof(kFnTable) / sizeof(kFnTable[0])));

    // Keep the handful RUNG 1 needs.
    if (void* pw = R::FindClass(L"PanelWidget")) {
        g_fnAddChild    = R::FindFunction(pw, L"AddChild");
        g_fnRemoveChild = R::FindFunction(pw, L"RemoveChild");
        g_fnChildCount  = R::FindFunction(pw, L"GetChildrenCount");
        g_fnGetChildAt  = R::FindFunction(pw, L"GetChildAt");
    }
    if (void* ws = R::FindClass(L"WidgetSwitcher")) {
        g_fnSetActiveIdx = R::FindFunction(ws, L"SetActiveWidgetIndex");
        g_fnGetActiveIdx = R::FindFunction(ws, L"GetActiveWidgetIndex");
    }
    if (void* w = R::FindClass(P::name::WidgetClass)) {
        g_fnDesiredSize = R::FindFunction(w, L"GetDesiredSize");
        g_fnIsHovered   = R::FindFunction(w, P::name::WidgetIsHoveredFn);
    }

    // A5 FIRST: it captures the live sub-screen children the donor pass prefers to read.
    MeasureSwitcher(menu);
    MeasureDonors(menu, "menu");

    // O5 + O8 on the one donor the shipped inject already uses.
    const int32_t bsOff = R::FindPropertyOffset(R::ClassOf(menu), P::name::UiMenuButtonStartProp);
    void* buttonStart   = ReadPtr(menu, bsOff);
    MeasureBrushHandles(buttonStart, L"ui_menu_C.button_start");
    MeasureDelegate(buttonStart, L"ui_menu_C.button_start");

    // A SECOND O5 DONOR, and it is the one that can actually answer the question. The
    // first run of this probe found ui_menu_C.button_start's four brushes carrying NO
    // ResourceObject, which makes its four zero handles uninformative -- a handle CACHES a
    // resource. ui_saveSlots_C's buttons are the ones section 7b measured onto
    // inst_uiButton, and the same run proved that sub-screen's widgets are RESIDENT at
    // menu time. So the art-bearing donor is readable right here, with no write at all --
    // which is what retired the rung that was going to show the screen to get at it.
    if (g_childSaveSlots) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(g_childSaveSlots), L"button_back");
        void* back = ReadPtr(g_childSaveSlots, off);
        MeasureBrushHandles(back, L"ui_saveSlots_C.button_back");
        MeasureDelegate(back, L"ui_saveSlots_C.button_back");
    }

    UE_LOGI("[native_ui_probe] ===== STAGE A done (rung1 %s) =====",
            WriteArmed() ? "ARMED -- it WRITES" : "not armed");
}

// =====================================================================================
// RUNG 1 -- the one WRITE. Does a hand-wired UUserWidget render inside the switcher?
// =====================================================================================
//
// THE EXPERIMENT HAS EXACTLY ONE VARIABLE. The widget built here is the SAME shape
// engine_widget's BuildTextWidget makes and pos_hud already ships through AddToViewport
// -- UUserWidget -> UWidgetTree -> UTextBlock root, from bare SpawnObject, never
// Initialize()d. That shape is PROVEN to render in the viewport. So if it does not render
// here, the difference is the switcher and nothing else. Composing an Overlay + UImage
// would have confounded the answer with our own tree.
//
// THE HOLD IS NOT OPTIONAL, and it is a deliberate deviation from the design text.
// Section 8 says restore "in the same tick". Measured against what an instrument can see:
// Slate lays out and paints AFTER our observer returns, so a same-tick restore presents no
// frame with our widget active and leaves GetDesiredSize reading the same zero it read
// before -- an instrument blind to the phenomenon always passes. The hold is therefore
// bounded by a deadline the PROBE owns (nothing the user must do), and the restore runs on
// every exit path below. That matters because at our index ESC is a no-op (section 8's
// OnKeyDown finding) and a throwaway has no button_back: a probe that could outlive its own
// deadline could strand the player in a menu with no way out.
enum class Rung1 : uint8_t { Idle, Held, Done, Failed };
Rung1 g_rung1 = Rung1::Idle;

constexpr unsigned long long kHoldMs   = 2200;  // several presented frames + one screenshot
constexpr unsigned long long kShotAtMs = 700;
constexpr unsigned long long kSampleMs = 250;
// SETTLE before the write. At boot the menu opens on the content-warning screen -- itself
// one of the switcher's children -- and the sub-screens are still being constructed. A
// rung that fired on the very first main-menu tick would be measuring a half-built menu
// and would take its screenshot over the warning rather than over the menu.
constexpr unsigned long long kSettleMs = 3000;
unsigned long long g_firstMainTickMs = 0;  // stamped on the first isPause==false tick

void* g_throwRoot    = nullptr;  // our UUserWidget (never Initialize()d)
void* g_throwText    = nullptr;  // its UTextBlock root
void* g_heldSwitcher = nullptr;
int32_t g_priorIndex = -1;
int32_t g_ourIndex   = -1;
unsigned long long g_holdStartMs  = 0;
unsigned long long g_lastSampleMs = 0;
bool g_shotTaken = false;

void Rung1Restore(const char* why) {
    if (!g_heldSwitcher) return;
    // Put the index back ONLY if it is still ours. The game's own sibling screens write
    // this field to navigate (ui_stats and ui_settings both do -- measured in their
    // ubergraphs), so restoring blindly would stomp a navigation the player just made.
    const int32_t now = CallIntNoArg(g_heldSwitcher, g_fnGetActiveIdx);
    if (now == g_ourIndex && g_fnSetActiveIdx) {
        ue_wrap::ParamFrame f(g_fnSetActiveIdx);
        f.Set<int32_t>(L"Index", g_priorIndex);
        Call(g_heldSwitcher, f);
        UE_LOGI("[native_ui_probe] RUNG1 restore (%s): activeIndex %d -> %d", why, g_ourIndex,
                g_priorIndex);
    } else {
        UE_LOGW("[native_ui_probe] RUNG1 restore (%s): activeIndex is %d, not ours (%d) -- the game "
                "navigated; leaving it alone", why, now, g_ourIndex);
    }
    if (g_throwRoot && g_fnRemoveChild) {
        ue_wrap::ParamFrame f(g_fnRemoveChild);
        f.Set<void*>(L"Content", g_throwRoot);
        Call(g_heldSwitcher, f);
        UE_LOGI("[native_ui_probe] RUNG1 RemoveChild -> children=%d (was %d with ours attached)",
                CallIntNoArg(g_heldSwitcher, g_fnChildCount), g_ourIndex + 1);
    }
    g_heldSwitcher = nullptr;
    g_throwRoot    = nullptr;
    g_throwText    = nullptr;
}

void Rung1Begin(void* menu) {
    if (!g_switcher || !g_fnAddChild || !g_fnSetActiveIdx || !g_fnGetActiveIdx || !g_fnChildCount) {
        UE_LOGW("[native_ui_probe] RUNG1 SKIPPED -- prerequisites unresolved (switcher=%p add=%p "
                "setIdx=%p getIdx=%p count=%p). O1's misses above name which.",
                g_switcher, g_fnAddChild, g_fnSetActiveIdx, g_fnGetActiveIdx, g_fnChildCount);
        g_rung1 = Rung1::Failed;
        return;
    }
    // Outer the widget to the switcher. The Outer does not root it -- what keeps a UMG
    // widget alive is the panel's `Slots` UPROPERTY array -- but it keeps the lifetime
    // question in one obvious place while it is attached.
    void* root = E::SpawnUObject(R::FindClass(P::name::UserWidgetClass), g_switcher);
    void* tree = root ? E::SpawnUObject(R::FindClass(P::name::WidgetTreeClass), root) : nullptr;
    void* txt  = tree ? E::SpawnUObject(R::FindClass(P::name::TextBlockClass), tree) : nullptr;
    if (!root || !tree || !txt) {
        UE_LOGE("[native_ui_probe] RUNG1 SpawnObject failed (root=%p tree=%p txt=%p)", root, tree,
                txt);
        g_rung1 = Rung1::Failed;
        return;
    }
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) =
        tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) =
        txt;
    E::SetWidgetText(txt, L"MULTIVOID NATIVE UMG PROBE -- RUNG 1");
    E::SetTextBlockColor(txt, ue_wrap::FLinearColor{1.f, 0.f, 1.f, 1.f});  // magenta: unmistakable

    // Baseline BEFORE attachment: a widget Slate has never taken has DesiredSize (0,0),
    // because UWidget::GetDesiredSize reads the underlying SWidget and answers zero when
    // there is none. That zero is what makes the post-hold read mean something.
    const ue_wrap::FVector2D pre = DesiredSizeOf(root);
    const int32_t nBefore = CallIntNoArg(g_switcher, g_fnChildCount);
    g_priorIndex = CallIntNoArg(g_switcher, g_fnGetActiveIdx);

    ue_wrap::ParamFrame add(g_fnAddChild);
    add.Set<void*>(L"Content", root);
    const bool added   = Call(g_switcher, add);
    void* slot         = add.Get<void*>(L"ReturnValue");
    const int32_t nAft = CallIntNoArg(g_switcher, g_fnChildCount);
    void* backSlot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UWidget_Slot);
    const ue_wrap::FVector2D postAdd = DesiredSizeOf(root);
    UE_LOGI("[native_ui_probe] RUNG1 AddChild called=%d slot=%p  UWidget::Slot back-ptr=%p  "
            "children %d -> %d  desiredSize pre=(%.1f,%.1f) postAdd=(%.1f,%.1f)  priorIndex=%d",
            added ? 1 : 0, slot, backSlot, nBefore, nAft, pre.X, pre.Y, postAdd.X, postAdd.Y,
            g_priorIndex);

    if (nAft != nBefore + 1) {
        UE_LOGE("[native_ui_probe] RUNG1 VERDICT: AddChild did NOT attach (child count unchanged). "
                "The 12th-child placement is DEAD; the browser falls back to AddToViewport.");
        g_rung1 = Rung1::Failed;  // nothing attached, and the index was never moved
        return;
    }

    g_ourIndex     = nAft - 1;
    g_heldSwitcher = g_switcher;
    g_throwRoot    = root;
    g_throwText    = txt;
    {
        ue_wrap::ParamFrame f(g_fnSetActiveIdx);
        f.Set<int32_t>(L"Index", g_ourIndex);
        Call(g_switcher, f);
    }
    UE_LOGW("[native_ui_probe] RUNG1 HOLD BEGIN on menu=%p: activeIndex %d -> %d (read back %d), "
            "auto-restore in %llu ms",
            menu, g_priorIndex, g_ourIndex, CallIntNoArg(g_switcher, g_fnGetActiveIdx), kHoldMs);
    g_holdStartMs  = ::GetTickCount64();
    g_lastSampleMs = 0;
    g_shotTaken    = false;
    g_rung1        = Rung1::Held;
}

void Rung1Tick() {
    const unsigned long long now  = ::GetTickCount64();
    const unsigned long long held = now - g_holdStartMs;
    // Bail out if anything about the object we are writing into changed under us.
    if (g_switcher != g_heldSwitcher) {
        Rung1Restore("switcher changed");
        UE_LOGW("[native_ui_probe] RUNG1 ABORTED -- the menu's switcher pointer moved mid-hold");
        g_rung1 = Rung1::Failed;
        return;
    }
    if (held >= kShotAtMs && !g_shotTaken) {
        g_shotTaken = true;
        UE_LOGI("[native_ui_probe] RUNG1 screenshot saved=%d (coop-screenshots/)",
                harness::screenshot::Capture(L"native-ui-probe-rung1") ? 1 : 0);
    }
    if (now - g_lastSampleMs >= kSampleMs) {
        g_lastSampleMs = now;
        const ue_wrap::FVector2D dRoot = DesiredSizeOf(g_throwRoot);
        const ue_wrap::FVector2D dTxt  = DesiredSizeOf(g_throwText);
        bool hovered = false;
        if (g_fnIsHovered && g_throwRoot) {
            ue_wrap::ParamFrame f(g_fnIsHovered);
            if (Call(g_throwRoot, f)) hovered = f.Get<bool>(L"ReturnValue");
        }
        UE_LOGI("[native_ui_probe] RUNG1 hold %4llums: root desiredSize=(%.1f,%.1f) text "
                "desiredSize=(%.1f,%.1f) rootHovered=%d | input_owner gameOwnsText=%d "
                "overlayOwnsText=%d foreground=%d",
                held, dRoot.X, dRoot.Y, dTxt.X, dTxt.Y, hovered ? 1 : 0,
                coop::input::input_owner::GameOwnsText() ? 1 : 0,
                coop::input::input_owner::OverlayOwnsText() ? 1 : 0,
                coop::input::input_owner::IsForeground() ? 1 : 0);
    }
    if (held < kHoldMs) return;

    const ue_wrap::FVector2D dRoot = DesiredSizeOf(g_throwRoot);
    const ue_wrap::FVector2D dTxt  = DesiredSizeOf(g_throwText);
    const bool laidOut = (dRoot.X > 0.f || dRoot.Y > 0.f) || (dTxt.X > 0.f || dTxt.Y > 0.f);
    Rung1Restore("deadline");
    if (laidOut)
        UE_LOGW("[native_ui_probe] RUNG1 VERDICT: RENDERS. A never-Initialize()d UUserWidget laid "
                "out inside the live UWidgetSwitcher (root=(%.1f,%.1f) text=(%.1f,%.1f)). The "
                "12th-child placement HOLDS -- confirm against the screenshot.",
                dRoot.X, dRoot.Y, dTxt.X, dTxt.Y);
    else
        UE_LOGE("[native_ui_probe] RUNG1 VERDICT: NO LAYOUT. AddChild attached but Slate never took "
                "the widget (desiredSize still zero after %llu ms active). The 12th-child placement "
                "is DEAD; the browser falls back to AddToViewport like every other surface we ship.",
                kHoldMs);
    g_rung1 = Rung1::Done;
}

// =====================================================================================
// The anchor -- our OWN post observer on ui_menu_C::Tick (the same anchor the shipped
// inject uses, so a null reading is attributable to a menu instance rather than to
// there being no menu at all).
// =====================================================================================

std::atomic<bool> g_installed{false};
std::atomic<bool> g_retrying{false};
void*   g_tickFn     = nullptr;
int32_t g_isPauseOff = -1;

void OnMenuTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!self || !Armed()) return;
    const bool isPause =
        (g_isPauseOff >= 0 && *(reinterpret_cast<uint8_t*>(self) + g_isPauseOff) != 0);

    // THE RESTORE OUTRANKS EVERY GATE BELOW IT. ui_menu_C is ONE instance -- the pause
    // menu is the same widget with isPause=true, sharing this same switcher_widgets. So a
    // hold that is still open when the player leaves for gameplay would put our throwaway
    // on screen the next time they press ESC, mid-game, with no button_back and with ESC
    // a no-op at our index (section 8's OnKeyDown finding). Restoring only from the MAIN
    // menu would leave that window open for exactly as long as the player stayed in the
    // world. So the hold is torn down on this very tick instead.
    if (g_rung1 == Rung1::Held && isPause) {
        Rung1Restore("pause menu opened mid-hold");
        UE_LOGW("[native_ui_probe] RUNG1 ABORTED -- the menu went to isPause during the hold; the "
                "throwaway is off the switcher and the index is back");
        g_rung1 = Rung1::Failed;
        return;
    }
    // MAIN menu only from here on.
    if (isPause) return;

    if (!g_stageADone || self != g_stageAMenu) {
        g_stageADone = true;
        g_stageAMenu = self;
        RunStageA(self);
    } else {
        // Keep the switcher pointer fresh for RUNG 1's abort check, without re-running the
        // census (this is a per-tick path).
        g_switcher = ReadPtr(self, g_switcherOff);
    }

    if (!WriteArmed()) return;
    if (!coop::dev_gate::Allowed()) return;  // house rule: no mutating dev path from a joined client
    if (g_firstMainTickMs == 0) g_firstMainTickMs = ::GetTickCount64();
    if (g_rung1 == Rung1::Idle && ::GetTickCount64() - g_firstMainTickMs < kSettleMs) return;
    switch (g_rung1) {
        case Rung1::Idle: Rung1Begin(self); break;
        case Rung1::Held: Rung1Tick();      break;
        default: break;  // Done / Failed: the rung runs once per process
    }
}

bool TryInstall() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(P::name::UiMenuClass);
    if (!cls) return false;  // menu BP not loaded yet -- caller retries
    g_tickFn     = R::FindFunction(cls, P::name::UiMenuTickFn);
    g_isPauseOff = R::FindPropertyOffset(cls, P::name::UiMenuIsPauseProp);
    if (!g_tickFn) return false;
    if (!GT::RegisterPostObserver(g_tickFn, &OnMenuTickPost)) {
        UE_LOGE("[native_ui_probe] RegisterPostObserver(ui_menu_C::Tick) failed -- table full?");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("[native_ui_probe] INSTALLED (tickFn=%p isPause@+0x%X); write rung %s", g_tickFn,
            static_cast<unsigned>(g_isPauseOff), WriteArmed() ? "ARMED" : "disarmed");
    return true;
}

DWORD WINAPI RetryThread(LPVOID) {
    for (int i = 0; i < 120 && !g_installed.load(std::memory_order_acquire); ++i) {
        GT::Post([] { TryInstall(); });
        ::Sleep(500);
    }
    g_retrying.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Init() {
    if (!Armed()) return;
    UE_LOGI("[native_ui_probe] ARMED (read census + RUNG 0). RUNG 1 (the write) %s.",
            WriteArmed() ? "ALSO ARMED" : "is OFF -- set [dev] native_ui_probe_write=1");
    GT::Post([] {
        if (!TryInstall() && !g_retrying.exchange(true)) {
            if (HANDLE t = ::CreateThread(nullptr, 0, &RetryThread, nullptr, 0, nullptr))
                ::CloseHandle(t);
            else
                g_retrying.store(false, std::memory_order_release);
        }
    });
}

}  // namespace coop::dev::native_ui_probe
