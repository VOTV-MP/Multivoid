// coop/dev/native_ui_probe.cpp -- the native UMG probe behind the server browser's design: a
// read-only census of the menu (the classes and UFunctions the browser needs, the switcher's
// child map, the style donors, the brush and delegate layouts) and, when armed to write, a
// throwaway widget held in the menu's switcher to measure layout, hover and GC survival. See
// coop/dev/native_ui_probe.h.

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

// Small helpers.

inline void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// Resolve one UFunction on the class that owns it and log the verdict; FindFunction does no
// super-walk, so the owning class is the question.
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

// Stage A: the read-only census, once per menu instance.

// Every class the browser instantiates and every UFunction it calls, with the owning class.
// Data, so a recook diff is a table diff.
struct FnRow { const wchar_t* cls; const wchar_t* fn; };
constexpr FnRow kFnTable[] = {
    // The panel API: one resolve on UPanelWidget serves every panel type, and UWidgetSwitcher has
    // no typed add of its own.
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
    // UWidget reads the hover and geometry path needs.
    {L"Widget",             L"IsHovered"},
    {L"Widget",             L"GetDesiredSize"},
    {L"Widget",             L"GetCachedGeometry"},
    {L"Widget",             L"SetVisibility"},
};

// Classes only instantiated (no function of their own to resolve).
constexpr const wchar_t* kClassOnly[] = {
    L"ScrollBox", L"Overlay", L"SizeBox", L"Image", L"HorizontalBox", L"EditableTextBox",
    L"CanvasPanel", L"WidgetSwitcher", L"UserWidget", L"WidgetTree", L"TextBlock",
};

// The style donors, with the class each is expected on. The question is whether they are
// resident and non-null at main-menu time: the styling rule is fail-closed, so a null donor
// means retry, never a default style.
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
    // Not a field on ui_saveSlots_C in this build's header dump; listed so the log says so.
    {L"ui_saveSlots_C", L"image_border",   "section-8 'image_border_*' -- expected ABSENT, confirming the doc row is wrong"},
};

// FScriptDelegate: a TWeakObjectPtr {int32 index, int32 serial} and an FName {int32, int32}.
struct ScriptDelegate {
    int32_t objectIndex;
    int32_t objectSerial;
    R::FName functionName;
};
// A multicast delegate property is a TArray<FScriptDelegate>: {ptr, num, max}.
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

// The FSlateResourceHandle question. FSlateBrush is 0x88 bytes and its reflected fields stop
// short of the trailing bitfield bools; the 16-byte gap between them -- kHandleOff below -- is an
// unreflected handle (a shared pointer). The button inject copies the whole 0x278 FButtonStyle,
// four brushes, so a populated handle would be aliased with no reference added.
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
        // ResourceObject is printed beside it: a brush with no resource cannot have a handle, so a
        // zero there would be uninformative.
        void* res = *reinterpret_cast<void**>(style + kBrushOff[i] + kResourceObjOff);
        if (res) ++withArt;
        UE_LOGI("[native_ui_probe] O5 %ls brush %-8s ResourceObject=%p  handle@+0x%zX = %016llX "
                "%016llX  -> %s",
                label, kName[i], res, kHandleOff, static_cast<unsigned long long>(lo),
                static_cast<unsigned long long>(hi), live ? "POPULATED" : "null");
    }
    // The verdict may not be stronger than the evidence: ui_menu_C.button_start's brushes carry no
    // ResourceObject, so their zero handles say nothing about a brush that has one; a handle caches
    // a resource.
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

// Read-only proof of the delegate layout: the game's own bound button confirms UButton::OnClicked
// at 0x3C8 and that a delegate-to-ProcessEvent path exists.
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

// The switcher's child map: which sub-screen sits at which index, and the live half of the
// placement argument (appending cannot renumber what is there). The live sub-screen instances
// are captured off the switcher's own child list, since FindObjectByClass can answer with a
// different instance, and the two are printed side by side.
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

// Donor residency. The ui_menu_C rows read the live menu; a sub-screen's owner is the switcher's
// own child from the map above, falling back to FindObjectByClass, and both pointers are
// printed: on this probe's first run FindObjectByClass returned a different non-CDO instance (a
// WidgetBlueprint carries a template tree not named Default__) and reported every sub-screen
// donor as null, while the live child's donors were all resident. The fallback is memoised per
// class name: it walks the whole GUObjectArray, and the table names one class six times.
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

    // The resolve census.
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

    // The handful the write rung needs.
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

    // The child map first: it captures the live sub-screen children the donor pass reads.
    MeasureSwitcher(menu);
    MeasureDonors(menu, "menu");

    // The brush and delegate reads on the donor the shipped inject uses.
    const int32_t bsOff = R::FindPropertyOffset(R::ClassOf(menu), P::name::UiMenuButtonStartProp);
    void* buttonStart   = ReadPtr(menu, bsOff);
    MeasureBrushHandles(buttonStart, L"ui_menu_C.button_start");
    MeasureDelegate(buttonStart, L"ui_menu_C.button_start");

    // A second brush donor, the one that can answer: ui_saveSlots_C's buttons carry art, and that
    // sub-screen's widgets are resident at menu time, so it is readable here with no write.
    if (g_childSaveSlots) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(g_childSaveSlots), L"button_back");
        void* back = ReadPtr(g_childSaveSlots, off);
        MeasureBrushHandles(back, L"ui_saveSlots_C.button_back");
        MeasureDelegate(back, L"ui_saveSlots_C.button_back");
    }

    UE_LOGI("[native_ui_probe] ===== STAGE A done (rung1 %s) =====",
            WriteArmed() ? "ARMED -- it WRITES" : "not armed");
}

// Rung 1, the one write: does a hand-wired UUserWidget render inside the switcher? One
// variable: the widget is the shape engine_widget's BuildTextWidget makes and the HUD ships
// through AddToViewport (UUserWidget, UWidgetTree, a root widget, from bare SpawnObject, never
// Initialize()d), proven to render in the viewport, so a failure here is the switcher and
// nothing else. The hold is not optional: Slate lays out and paints after our observer
// returns, so a same-tick restore would present no frame and read the same zero size. The hold
// is bounded by a deadline the probe owns, and the restore runs on every exit path: at our
// index ESC is a no-op and a throwaway has no back button, so an outlived hold could strand the
// player.
enum class Rung1 : uint8_t { Idle, Held, Done, Failed };
Rung1 g_rung1 = Rung1::Idle;

// Rung 2 rides the same hold and answers the two questions the browser's hit-test and lifetime
// rest on. Hover: does a bare UImage with Visibility=Visible answer IsHovered? Rung 1's root
// read hovered on every sample, which a full-bleed widget under an unmoved cursor cannot
// distinguish from always-true, so rung 2 uses a bounded target and tests both directions. GC:
// does the subtree survive a purge? The UPROPERTY chain from the live switcher (WidgetTree,
// RootWidget, Slots, Content) says it is reachable, so AddToRoot would be wrong, but rung 1
// lived one tick and never met a collection. The move and the sample are separate phases:
// Slate processes the mouse move later in the frame than our observer runs, so a sample in the
// move's tick reads the previous position.
constexpr unsigned long long kHoldMs   = 5800;  // phases below + slack, then restore
constexpr unsigned long long kShotAtMs = 700;
constexpr unsigned long long kSampleMs = 250;
constexpr unsigned long long kHoverOnMoveMs   = 1400;  // cursor -> inside the bounded image
constexpr unsigned long long kHoverOnSampleMs = 2000;  // ...read it SEVERAL ticks later
constexpr unsigned long long kHoverOffMoveMs   = 2800;  // cursor -> client centre (outside it)
constexpr unsigned long long kHoverOffSampleMs = 3400;
constexpr unsigned long long kGcAtMs     = 4200;  // ForceGarbageCollection, then re-assert
constexpr unsigned long long kGcCheckMs  = 5000;
// The bounded hit-test target, in Slate units at the switcher's top-left; a full-bleed target
// makes a true reading unfalsifiable.
constexpr float kProbeImgW = 400.f;
constexpr float kProbeImgH = 64.f;   // the native row height
// Settle before the write: at boot the menu opens on the content-warning screen, itself a
// switcher child, while the sub-screens are still being constructed.
constexpr unsigned long long kSettleMs = 3000;
unsigned long long g_firstMainTickMs = 0;  // stamped on the first isPause==false tick

void* g_throwRoot    = nullptr;  // our UUserWidget (never Initialize()d)
void* g_throwText    = nullptr;  // a UTextBlock -- the VISUAL proof, kept from RUNG 1
void* g_throwOverlay = nullptr;  // the root widget: a UOverlay (RUNG 2's deeper tree)
void* g_throwImage   = nullptr;  // the bounded UImage -- RUNG 2's hit-test SUBJECT
void* g_throwSizeBox = nullptr;  // bounds the image (also exercises the bOverride bitfield)
void* g_heldSwitcher = nullptr;
int32_t g_priorIndex = -1;
int32_t g_ourIndex   = -1;
unsigned long long g_holdStartMs  = 0;
unsigned long long g_lastSampleMs = 0;
bool g_shotTaken = false;
// The phase latches and results. -1 is not sampled, so an unrun phase can never read as a
// negative answer.
int  g_hoverOnImg = -1, g_hoverOnText = -1, g_hoverOnRoot = -1;
int  g_hoverOffImg = -1, g_hoverOffRoot = -1;
int  g_hoverFgOn = -1, g_hoverFgOff = -1;   // was OUR window foreground at each sample
bool g_didHoverOnMove = false, g_didHoverOffMove = false;
bool g_didHoverOn = false, g_didHoverOff = false, g_didGc = false, g_didGcCheck = false;
int  g_gcChildCount = -1;
ue_wrap::FVector2D g_gcRootSize{0.f, 0.f}, g_gcImgSize{0.f, 0.f};

// The game's own top-level window, found from the game thread that created it. Deterministic
// where the foreground window is not: an unattended run can have another window focused, and
// that must be told apart from a hit-test failure.
BOOL CALLBACK PickThreadWindow(HWND h, LPARAM lp) {
    if (!::IsWindowVisible(h)) return TRUE;
    RECT rc{};
    if (!::GetClientRect(h, &rc) || rc.right - rc.left < 64 || rc.bottom - rc.top < 64) return TRUE;
    *reinterpret_cast<HWND*>(lp) = h;
    return FALSE;  // first match wins
}
HWND GameWindow() {
    HWND found = nullptr;
    ::EnumThreadWindows(::GetCurrentThreadId(), &PickThreadWindow,
                        reinterpret_cast<LPARAM>(&found));
    return found;
}

// Move the OS cursor to a client-space point and verify it landed: SetCursorPos is silently
// clamped by whatever ClipCursor rect is live, and an unverified move would turn a clamp into a
// false "not hovered".
bool MoveCursorClient(HWND hwnd, int cx, int cy, const char* why) {
    if (!hwnd) return false;
    POINT p{cx, cy};
    if (!::ClientToScreen(hwnd, &p)) return false;
    ::SetCursorPos(p.x, p.y);
    POINT got{};
    ::GetCursorPos(&got);
    const bool ok = (got.x == p.x && got.y == p.y);
    UE_LOGI("[native_ui_probe] RUNG2 cursor %s -> client(%d,%d) screen(%ld,%ld) got(%ld,%ld) %s",
            why, cx, cy, p.x, p.y, got.x, got.y, ok ? "OK" : "CLAMPED -- reading is UNTRUSTED");
    return ok;
}

bool HoveredOf(void* w) {
    if (!w || !g_fnIsHovered) return false;
    ue_wrap::ParamFrame f(g_fnIsHovered);
    if (!Call(w, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

void Rung1Restore(const char* why) {
    if (!g_heldSwitcher) return;
    // The index goes back only if it is still ours: the game's sibling screens write it to
    // navigate, and a blind restore would stomp a navigation the player just made.
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
    g_throwOverlay = nullptr;
    g_throwImage   = nullptr;
    g_throwSizeBox = nullptr;
}

void Rung1Begin(void* menu) {
    if (!g_switcher || !g_fnAddChild || !g_fnSetActiveIdx || !g_fnGetActiveIdx || !g_fnChildCount) {
        UE_LOGW("[native_ui_probe] RUNG1 SKIPPED -- prerequisites unresolved (switcher=%p add=%p "
                "setIdx=%p getIdx=%p count=%p). O1's misses above name which.",
                g_switcher, g_fnAddChild, g_fnSetActiveIdx, g_fnGetActiveIdx, g_fnChildCount);
        g_rung1 = Rung1::Failed;
        return;
    }
    // Outered to the switcher. The Outer does not root it (the panel's Slots array does); it keeps
    // the lifetime question in one place.
    void* root = E::SpawnUObject(R::FindClass(P::name::UserWidgetClass), g_switcher);
    void* tree = root ? E::SpawnUObject(R::FindClass(P::name::WidgetTreeClass), root) : nullptr;
    // The root is a UOverlay, the browser's real shape (a scrim image and content), so a deeper
    // hand-built tree failing to lay out is itself the finding.
    void* ovl  = tree ? E::SpawnUObject(R::FindClass(L"Overlay"), tree) : nullptr;
    void* sbox = ovl ? E::SpawnUObject(R::FindClass(L"SizeBox"), ovl) : nullptr;
    void* img  = sbox ? E::SpawnUObject(R::FindClass(L"Image"), sbox) : nullptr;
    void* txt  = ovl ? E::SpawnUObject(R::FindClass(P::name::TextBlockClass), ovl) : nullptr;
    if (!root || !tree || !ovl || !sbox || !img || !txt) {
        UE_LOGE("[native_ui_probe] RUNG1/2 SpawnObject failed (root=%p tree=%p overlay=%p "
                "sizeBox=%p image=%p txt=%p)", root, tree, ovl, sbox, img, txt);
        g_rung1 = Rung1::Failed;
        return;
    }
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) =
        tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) =
        ovl;
    E::SetWidgetText(txt, L"MULTIVOID NATIVE UMG PROBE -- RUNG 1/2");
    E::SetTextBlockColor(txt, ue_wrap::FLinearColor{1.f, 0.f, 1.f, 1.f});  // magenta: unmistakable

    // The hit-test subject. A UImage with no ResourceObject and only a tint draws a solid rect (the
    // game's own save-slot scrim is exactly that), so this needs no donor and no texture.
    {
        auto* b = reinterpret_cast<uint8_t*>(img) + P::off::UImage_Brush;
        *reinterpret_cast<ue_wrap::FLinearColor*>(b + P::off::FSlateBrush_TintColor) =
            ue_wrap::FLinearColor{0.f, 0.9f, 0.9f, 0.85f};  // cyan, clearly ours
        *(b + P::off::FSlateBrush_TintColor + P::off::FSlateColor_ColorUseRule) = 0;  // Specified
    }
    // Bounded through the SizeBox's UFunctions: the override values are plain fields, but the
    // bOverride bits are a bitfield, and a raw write would silently do nothing.
    if (void* sbCls = R::FindClass(L"SizeBox")) {
        if (void* fnH = R::FindFunction(sbCls, L"SetHeightOverride")) {
            ue_wrap::ParamFrame f(fnH); f.Set<float>(L"InHeightOverride", kProbeImgH); Call(sbox, f);
        }
        if (void* fnW = R::FindFunction(sbCls, L"SetWidthOverride")) {
            ue_wrap::ParamFrame f(fnW); f.Set<float>(L"InWidthOverride", kProbeImgW); Call(sbox, f);
        }
    }
    if (void* cwCls = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fnSet = R::FindFunction(cwCls, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fnSet); f.Set<void*>(L"Content", img); Call(sbox, f);
        }
    }
    // Both children pinned top-left and bottom-left, so the image occupies a known rect and the
    // text cannot sit on it. Left is 1; Top 1, Bottom 3.
    if (void* ovlCls = R::FindClass(L"Overlay")) {
        if (void* fnAdd = R::FindFunction(ovlCls, L"AddChildToOverlay")) {
            auto place = [&](void* child, uint8_t h, uint8_t v) {
                ue_wrap::ParamFrame f(fnAdd);
                f.Set<void*>(L"Content", child);
                if (!Call(ovl, f)) return;
                if (void* slot = f.Get<void*>(L"ReturnValue")) {
                    auto* s = reinterpret_cast<uint8_t*>(slot);
                    *(s + P::off::UOverlaySlot_HAlign) = h;
                    *(s + P::off::UOverlaySlot_VAlign) = v;
                }
            };
            place(sbox, 1, 1);  // Left / Top   -- the bounded hit-test target
            place(txt,  1, 3);  // Left / Bottom-- the visual proof, clear of the target
        }
    }
    // Visible (0) is required for the image to answer IsHovered; a self-hit-test-invisible image
    // reads false.
    E::SetWidgetVisibility(img, 0);

    // The baseline before attachment: a widget Slate has never taken has DesiredSize (0,0), which
    // is what makes the post-hold read mean something.
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
    // Published only after a successful attach, so a failed add leaves the phase machine no
    // pointers the restore would never see.
    g_throwOverlay = ovl;
    g_throwImage   = img;
    g_throwSizeBox = sbox;
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
    // Bail out if the object being written into changed under us.
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
        const ue_wrap::FVector2D dImg  = DesiredSizeOf(g_throwImage);
        UE_LOGI("[native_ui_probe] RUNG1 hold %4llums: root desiredSize=(%.1f,%.1f) image "
                "desiredSize=(%.1f,%.1f) rootHovered=%d imgHovered=%d | input_owner gameOwnsText=%d "
                "overlayOwnsText=%d foreground=%d",
                held, dRoot.X, dRoot.Y, dImg.X, dImg.Y, HoveredOf(g_throwRoot) ? 1 : 0,
                HoveredOf(g_throwImage) ? 1 : 0,
                coop::input::input_owner::GameOwnsText() ? 1 : 0,
                coop::input::input_owner::OverlayOwnsText() ? 1 : 0,
                coop::input::input_owner::IsForeground() ? 1 : 0);
    }

    // The rung 2 phases. Each fires once, in order, and none ends the hold early; the restore at
    // the deadline is the only exit.
    if (held >= kHoverOnMoveMs && !g_didHoverOnMove) {
        g_didHoverOnMove = true;
        HWND hwnd = GameWindow();
        if (hwnd) {
            // Inside the bounded image: its rect is the switcher's top-left corner (the switcher
            // fills the screen), aimed a quarter in so a DPI scale other than 1 still lands inside.
            ::SetForegroundWindow(hwnd);
            MoveCursorClient(hwnd, static_cast<int>(kProbeImgW / 4),
                             static_cast<int>(kProbeImgH / 4), "ON the image");
        } else {
            UE_LOGW("[native_ui_probe] RUNG2 hover-ON: no game window found -- phase UNRUN");
        }
    }
    if (held >= kHoverOnSampleMs && !g_didHoverOn) {
        g_didHoverOn  = true;
        g_hoverFgOn   = coop::input::input_owner::IsForeground() ? 1 : 0;
        g_hoverOnImg  = HoveredOf(g_throwImage) ? 1 : 0;
        g_hoverOnText = HoveredOf(g_throwText) ? 1 : 0;
        g_hoverOnRoot = HoveredOf(g_throwRoot) ? 1 : 0;
        UE_LOGW("[native_ui_probe] RUNG2 hover-ON sample (%llums after the move): image=%d text=%d "
                "root=%d foreground=%d",
                kHoverOnSampleMs - kHoverOnMoveMs, g_hoverOnImg, g_hoverOnText, g_hoverOnRoot,
                g_hoverFgOn);
    }
    if (held >= kHoverOffMoveMs && !g_didHoverOffMove) {
        g_didHoverOffMove = true;
        HWND hwnd = GameWindow();
        RECT cr{};
        if (hwnd && ::GetClientRect(hwnd, &cr)) {
            MoveCursorClient(hwnd, (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2,
                             "OFF the image (client centre)");
        } else {
            UE_LOGW("[native_ui_probe] RUNG2 hover-OFF: no game window found -- phase UNRUN");
        }
    }
    if (held >= kHoverOffSampleMs && !g_didHoverOff) {
        g_didHoverOff  = true;
        g_hoverFgOff   = coop::input::input_owner::IsForeground() ? 1 : 0;
        g_hoverOffImg  = HoveredOf(g_throwImage) ? 1 : 0;
        g_hoverOffRoot = HoveredOf(g_throwRoot) ? 1 : 0;
        UE_LOGW("[native_ui_probe] RUNG2 hover-OFF sample (%llums after the move): image=%d root=%d "
                "foreground=%d",
                kHoverOffSampleMs - kHoverOffMoveMs, g_hoverOffImg, g_hoverOffRoot, g_hoverFgOff);
    }
    if (held >= kGcAtMs && !g_didGc) {
        g_didGc = true;
        // The subtree is reachable only through UPROPERTYs from the live switcher, with no
        // AddToRoot; a forced collection is where that argument becomes a measurement.
        const bool ran = E::ForceGarbageCollection();
        UE_LOGW("[native_ui_probe] RUNG2 GC: ForceGarbageCollection ran=%d -- re-asserting in %llums",
                ran ? 1 : 0, kGcCheckMs - kGcAtMs);
    }
    if (held >= kGcCheckMs && !g_didGcCheck) {
        g_didGcCheck  = true;
        g_gcChildCount = CallIntNoArg(g_heldSwitcher, g_fnChildCount);
        g_gcRootSize   = DesiredSizeOf(g_throwRoot);
        g_gcImgSize    = DesiredSizeOf(g_throwImage);
        const bool survived = (g_gcChildCount == g_ourIndex + 1) &&
                              (g_gcRootSize.X > 0.f || g_gcRootSize.Y > 0.f);
        if (survived)
            UE_LOGW("[native_ui_probe] RUNG2 GC VERDICT: SURVIVED. children=%d root=(%.1f,%.1f) "
                    "image=(%.1f,%.1f) -- the UPROPERTY chain from the switcher roots the whole "
                    "subtree; AddToRoot is NOT needed and would be wrong.",
                    g_gcChildCount, g_gcRootSize.X, g_gcRootSize.Y, g_gcImgSize.X, g_gcImgSize.Y);
        else
            UE_LOGE("[native_ui_probe] RUNG2 GC VERDICT: DID NOT SURVIVE. children=%d (expected %d) "
                    "root=(%.1f,%.1f) -- the browser's subtree needs an explicit GC pin + a paired "
                    "un-root (reflection.h:109), and the pool-in-the-panel design is WRONG.",
                    g_gcChildCount, g_ourIndex + 1, g_gcRootSize.X, g_gcRootSize.Y);
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

    // The hover verdict, three-valued: a phase that never ran, or a sample taken while another
    // application owned the foreground, is inconclusive, never a negative.
    if (g_hoverOnImg < 0 || g_hoverOffImg < 0) {
        UE_LOGE("[native_ui_probe] RUNG2 HOVER VERDICT: INCONCLUSIVE -- a phase did not run "
                "(on=%d off=%d). The browser's hit-test design is UNMEASURED; do not build on it.",
                g_hoverOnImg, g_hoverOffImg);
    } else if (g_hoverFgOn != 1 || g_hoverFgOff != 1) {
        UE_LOGE("[native_ui_probe] RUNG2 HOVER VERDICT: INCONCLUSIVE -- our window was not "
                "foreground at sample time (fgOn=%d fgOff=%d, image on=%d off=%d). Slate only "
                "tracks the pointer for the active application, so this says nothing either way.",
                g_hoverFgOn, g_hoverFgOff, g_hoverOnImg, g_hoverOffImg);
    } else if (g_hoverOnImg == 1 && g_hoverOffImg == 0) {
        UE_LOGW("[native_ui_probe] RUNG2 HOVER VERDICT: IsHovered() ANSWERS. A bare UImage with "
                "Visibility=Visible read hovered=1 with the cursor inside its %.0fx%.0f rect and "
                "hovered=0 with the cursor outside it (root on=%d off=%d, text on=%d). The "
                "browser's row / chrome / scrim hit-test holds.",
                kProbeImgW, kProbeImgH, g_hoverOnRoot, g_hoverOffRoot, g_hoverOnText);
    } else if (g_hoverOnImg == 1 && g_hoverOffImg == 1) {
        UE_LOGE("[native_ui_probe] RUNG2 HOVER VERDICT: ALWAYS-TRUE. The image read hovered=1 both "
                "inside AND outside its rect -- IsHovered() is not discriminating here, exactly the "
                "suspicion RUNG 1's rootHovered=1 raised. The browser CANNOT hit-test by IsHovered; "
                "it needs a measured alternative before any of P2 is written.");
    } else {
        UE_LOGE("[native_ui_probe] RUNG2 HOVER VERDICT: NEVER TRUE (on=%d off=%d). A bare UImage "
                "does not answer IsHovered() even with the cursor inside it and Visibility=Visible. "
                "The row hit-test needs a UButton (or another mechanism) -- section 8's 'no row "
                "button' rejection must be re-opened before P2 is written.",
                g_hoverOnImg, g_hoverOffImg);
    }
    g_rung1 = Rung1::Done;
}

// The anchor: our own post observer on ui_menu_C::Tick, the shipped inject's anchor too, so a
// null reading is attributable to a menu instance rather than to no menu at all.

std::atomic<bool> g_installed{false};
std::atomic<bool> g_retrying{false};
void*   g_tickFn     = nullptr;
int32_t g_isPauseOff = -1;

void OnMenuTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!self || !Armed()) return;
    const bool isPause =
        (g_isPauseOff >= 0 && *(reinterpret_cast<uint8_t*>(self) + g_isPauseOff) != 0);

    // The restore outranks every gate below. ui_menu_C is one instance, and the pause menu is the
    // same widget with isPause set, sharing the switcher; a hold still open when the player leaves
    // for gameplay would put the throwaway on screen at the next ESC, with no back button and ESC
    // a no-op at our index. So the hold is torn down on this tick.
    if (g_rung1 == Rung1::Held && isPause) {
        Rung1Restore("pause menu opened mid-hold");
        UE_LOGW("[native_ui_probe] RUNG1 ABORTED -- the menu went to isPause during the hold; the "
                "throwaway is off the switcher and the index is back");
        g_rung1 = Rung1::Failed;
        return;
    }
    // The main menu only from here on.
    if (isPause) return;

    if (!g_stageADone || self != g_stageAMenu) {
        g_stageADone = true;
        g_stageAMenu = self;
        RunStageA(self);
    } else {
        // The switcher pointer kept fresh for the abort check, without re-running the census.
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
