// ue_wrap/engine/umg_build.cpp -- see ue_wrap/engine/umg_build.h.

#include "ue_wrap/engine/umg_build.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"  // GetWorldContext -- the Slate library calls are static

#include <cstring>
#include <vector>

namespace ue_wrap::umg {
namespace {

namespace P = profile;
namespace R = reflection;

// Resolved once per (class, function) pair. Classes and UFunctions never move within a
// process, so a null after a successful resolve is impossible and a null before one is a
// recook problem -- which is why every miss below LOGS rather than failing silently.
struct FnCache {
    const wchar_t* cls;
    const wchar_t* fn;
    void*          ptr;
    bool           tried;
};

void* Resolve(FnCache& c) {
    if (c.ptr || c.tried) return c.ptr;
    c.tried = true;
    void* k = R::FindClass(c.cls);
    if (!k) {
        UE_LOGE("umg: class '%ls' not found -- '%ls' is unavailable this session", c.cls, c.fn);
        return nullptr;
    }
    c.ptr = R::FindFunction(k, c.fn);
    if (!c.ptr)
        UE_LOGE("umg: %ls::%ls unresolved -- FindFunction matches the OWNING class only "
                "(reflection.cpp:493); is this the right owner after a recook?", c.cls, c.fn);
    return c.ptr;
}

// UPanelWidget owns the whole generic panel API -- one resolve serves every panel type.
FnCache g_addChild   {L"PanelWidget",   L"AddChild",         nullptr, false};
FnCache g_removeChild{L"PanelWidget",   L"RemoveChild",      nullptr, false};
FnCache g_childCount {L"PanelWidget",   L"GetChildrenCount", nullptr, false};
FnCache g_childAt    {L"PanelWidget",   L"GetChildAt",       nullptr, false};
FnCache g_childIndex {L"PanelWidget",   L"GetChildIndex",    nullptr, false};
FnCache g_swSetIdx   {L"WidgetSwitcher", L"SetActiveWidgetIndex", nullptr, false};
FnCache g_swGetIdx   {L"WidgetSwitcher", L"GetActiveWidgetIndex", nullptr, false};
FnCache g_imgTint    {L"Image",          L"SetBrushTintColor",    nullptr, false};
FnCache g_sbHeight   {L"SizeBox",        L"SetHeightOverride",    nullptr, false};
FnCache g_sbWidth    {L"SizeBox",        L"SetWidthOverride",     nullptr, false};
FnCache g_setClip    {L"Widget",         L"SetClipping",          nullptr, false};
FnCache g_scrollSet  {L"ScrollBox",      L"SetScrollOffset",      nullptr, false};
FnCache g_scrollGet  {L"ScrollBox",      L"GetScrollOffset",      nullptr, false};
FnCache g_scrollEnd  {L"ScrollBox",      L"GetScrollOffsetOfEnd", nullptr, false};
FnCache g_scrollFrac {L"ScrollBox",      L"GetViewOffsetFraction", nullptr, false};
FnCache g_cachedGeom {L"Widget",         L"GetCachedGeometry",    nullptr, false};
FnCache g_getVis     {L"Widget",         L"GetVisibility",        nullptr, false};
FnCache g_getParent  {L"Widget",         L"GetParent",            nullptr, false};
FnCache g_localSize  {L"SlateBlueprintLibrary", L"GetLocalSize",   nullptr, false};
FnCache g_localToAbs {L"SlateBlueprintLibrary", L"LocalToAbsolute", nullptr, false};
FnCache g_screenToAbs{L"SlateBlueprintLibrary", L"ScreenToWidgetAbsolute", nullptr, false};
FnCache g_setContent {L"ContentWidget",  L"SetContent",           nullptr, false};

}  // namespace

// UContentWidget::SetContent -- the ONE owner, and it is latched.
//
// This existed as SEVEN copies of the same six-line resolve-then-call, none of them latched,
// and `R::FindFunction` has no result cache: it walks the whole GUObjectArray. Two of the
// seven sat inside `BuildRow`, i.e. once PER ROW -- so filling a 64-row list did 64 full
// array walks in a single frame, and opening the hosting window did 24 on the frame the
// player pressed the button. The post-ship perf audit priced that at 70-102 ms and 26-38 ms
// respectively, against an 8.5 ms budget.
//
// The bitter part: a previous fix had already latched `GetContent` in the same file and left
// `SetContent` alone -- the reader was cured and the two writers were not. One function with
// one cache is the shape that cannot rot that way.
bool SetContent(void* contentWidget, void* child) {
    void* fn = Resolve(g_setContent);
    if (!contentWidget || !child || !fn) return false;
    ParamFrame f(fn);
    f.Set<void*>(L"Content", child);
    return Call(contentWidget, f);
}

void* AddChild(void* panel, void* child) {
    void* fn = Resolve(g_addChild);
    if (!panel || !child || !fn) return nullptr;
    ParamFrame f(fn);
    f.Set<void*>(L"Content", child);
    if (!Call(panel, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

bool RemoveChild(void* panel, void* child) {
    void* fn = Resolve(g_removeChild);
    if (!panel || !child || !fn) return false;
    ParamFrame f(fn);
    f.Set<void*>(L"Content", child);
    if (!Call(panel, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

int32_t ChildCount(void* panel) {
    void* fn = Resolve(g_childCount);
    if (!panel || !fn) return -1;
    ParamFrame f(fn);
    if (!Call(panel, f)) return -1;
    return f.Get<int32_t>(L"ReturnValue");
}

void* ChildAt(void* panel, int32_t index) {
    void* fn = Resolve(g_childAt);
    if (!panel || !fn || index < 0) return nullptr;
    ParamFrame f(fn);
    f.Set<int32_t>(L"Index", index);
    if (!Call(panel, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

int32_t IndexOfChild(void* panel, void* child) {
    void* fn = Resolve(g_childIndex);
    if (!panel || !child || !fn) return -1;
    ParamFrame f(fn);
    f.Set<void*>(L"Content", child);
    if (!Call(panel, f)) return -1;
    return f.Get<int32_t>(L"ReturnValue");
}

bool SwitcherSetIndex(void* switcher, int32_t index) {
    void* fn = Resolve(g_swSetIdx);
    if (!switcher || !fn || index < 0) return false;
    ParamFrame f(fn);
    f.Set<int32_t>(L"Index", index);
    return Call(switcher, f);
}

// The parameter name is NewScrollOffset, read from the CXXHeaderDump (UMG.hpp:1198) --
// ParamFrame resolves by name off the live FProperty chain, so a wrong name is a silent
// no-op write into a zeroed frame, i.e. SetScrollOffset(0) whatever you asked for.
bool SetScrollOffset(void* scrollBox, float offset) {
    void* fn = Resolve(g_scrollSet);
    if (!scrollBox || !fn) return false;
    ParamFrame f(fn);
    if (!f.Set<float>(L"NewScrollOffset", offset)) return false;
    return Call(scrollBox, f);
}

bool ScrollOffset(void* scrollBox, float& out) {
    void* fn = Resolve(g_scrollGet);
    if (!scrollBox || !fn) return false;
    ParamFrame f(fn);
    if (!Call(scrollBox, f)) return false;
    out = f.Get<float>(L"ReturnValue");
    return true;
}

bool ScrollOffsetOfEnd(void* scrollBox, float& out) {
    void* fn = Resolve(g_scrollEnd);
    if (!scrollBox || !fn) return false;
    ParamFrame f(fn);
    if (!Call(scrollBox, f)) return false;
    out = f.Get<float>(L"ReturnValue");
    return true;
}

void CloneButtonStyle(void* dstButton, void* srcButton) {
    if (!dstButton || !srcButton) return;
    auto* d = reinterpret_cast<uint8_t*>(dstButton);
    auto* s = reinterpret_cast<uint8_t*>(srcButton);
    // FButtonStyle embeds FOUR FSlateBrushes, each with an unreflected
    // FSlateResourceHandle at +0x70; CloneStyle zeroes all four.
    CloneStyle(d, P::off::UButton_WidgetStyle, s, P::off::UButton_WidgetStyle,
               P::off::FButtonStyle_Size, P::off::FButtonStyleBrushes, 4);
    // KEEP each FSlateSound's ResourceObject (@ 0x00) so the button plays the native
    // press + hover sounds; zero ONLY the trailing TSharedPtr cache.
    std::memset(d + P::off::UButton_WidgetStyle + P::off::FButtonStyle_PressedSlateSound +
                P::off::FSlateSound_CacheStart, 0,
                P::off::FSlateSound_Size - P::off::FSlateSound_CacheStart);
    std::memset(d + P::off::UButton_WidgetStyle + P::off::FButtonStyle_HoveredSlateSound +
                P::off::FSlateSound_CacheStart, 0,
                P::off::FSlateSound_Size - P::off::FSlateSound_CacheStart);
    *reinterpret_cast<FLinearColor*>(d + P::off::UButton_ColorAndOpacity) =
        *reinterpret_cast<FLinearColor*>(s + P::off::UButton_ColorAndOpacity);
    *reinterpret_cast<FLinearColor*>(d + P::off::UButton_BackgroundColor) =
        *reinterpret_cast<FLinearColor*>(s + P::off::UButton_BackgroundColor);
}

void LogVisibilityChain(const char* tag, void* widget) {
    void* visFn = Resolve(g_getVis);
    void* parFn = Resolve(g_getParent);
    if (!widget || !visFn || !parFn) return;
    // ESlateVisibility, and the two that matter are neighbours in the enum, which is how
    // this class of bug hides: HitTestInvisible(3) takes the whole subtree out of the hit
    // grid, SelfHitTestInvisible(4) takes only the widget itself and is what a container
    // wants. A chain that paints correctly tells you nothing about which one it carries.
    static const char* kNames[] = {"Visible", "Collapsed", "Hidden",
                                   "HitTestInvisible", "SelfHitTestInvisible"};
    void* w = widget;
    for (int depth = 0; w && depth < 12; ++depth) {
        uint8_t vis = 255;
        {
            ParamFrame f(visFn);
            if (Call(w, f)) vis = f.Get<uint8_t>(L"ReturnValue");
        }
        UE_LOGW("umg: %s chain[%d] %ls '%ls' visibility=%u (%s)", tag, depth,
                R::ClassNameOf(w).c_str(), R::ToString(R::NameOf(w)).c_str(), vis,
                vis < 5 ? kNames[vis] : "UNREAD");
        ParamFrame p(parFn);
        w = Call(w, p) ? p.Get<void*>(L"ReturnValue") : nullptr;
    }
}

// THE CURSOR, IN THE SAME SPACE THE RECTS ARE IN -- asked of Slate, not derived.
//
// WHY IT IS NOT A SUBTRACTION. `WidgetScreenRect` returns whatever
// `LocalToAbsolute` returns, and the relationship between that and the OS cursor
// is not a fact this file gets to assume: it involves the window's client origin
// AND the viewport's UI scale, and this project has now been wrong about it twice
// in one day in OPPOSITE directions -- first comparing desktop pixels to the rects
// directly (measured off by the client origin, 320x180 on the lab rig), then
// subtracting only the origin, after which a user still reported an offset and in
// the other direction. Two hand-derived corrections and two wrong answers is the
// point at which you stop deriving and ask the engine.
//
// `ScreenToWidgetAbsolute` is Slate's own inverse of the transform that produced
// those rects, so whatever the scale and wherever the window sits, both sides of
// the comparison come from the same source. `screenPos` is VIEWPORT/client pixels
// (do the ScreenToClient first); `bIncludeWindowPosition=false` because the rects
// we compare against are the plain LocalToAbsolute ones.
//
// Returns false if the function or the context is unavailable, leaving `out`
// untouched -- the caller decides what a missing conversion means rather than
// silently receiving an unconverted point.
bool CursorToWidgetAbsolute(const FVector2D& screenPos, FVector2D& out) {
    void* fn = Resolve(g_screenToAbs);
    if (!fn) return false;
    static void* const sLib = [] { return R::FindClassDefaultObject(L"SlateBlueprintLibrary"); }();
    if (!sLib) return false;
    void* ctx = ue_wrap::engine::GetWorldContext();
    if (!ctx) return false;

    ParamFrame f(fn);
    // Named, never positional: a signature change must fail loudly rather than
    // write a bool into a float pair.
    if (f.ParamOffset(L"ScreenPosition") < 0 || f.ParamOffset(L"AbsoluteCoordinate") < 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            UE_LOGE("umg: ScreenToWidgetAbsolute has an unexpected signature (ScreenPosition@%d "
                    "AbsoluteCoordinate@%d) -- refusing to guess the frame layout",
                    f.ParamOffset(L"ScreenPosition"), f.ParamOffset(L"AbsoluteCoordinate"));
        }
        return false;
    }
    f.Set<void*>(L"WorldContextObject", ctx);
    f.Set<FVector2D>(L"ScreenPosition", screenPos);
    if (f.ParamOffset(L"bIncludeWindowPosition") >= 0)
        f.Set<bool>(L"bIncludeWindowPosition", false);
    if (!Call(sLib, f)) return false;
    out = f.Get<FVector2D>(L"AbsoluteCoordinate");
    return true;
}

bool WidgetScreenRect(void* widget, FVector2D& outTopLeft, FVector2D& outSize) {
    void* geomFn = Resolve(g_cachedGeom);
    void* sizeFn = Resolve(g_localSize);
    void* absFn  = Resolve(g_localToAbs);
    if (!widget || !geomFn || !sizeFn || !absFn) return false;

    // The library's functions are static, so they dispatch on the CDO -- the same shape
    // the engine's own BlueprintFunctionLibrary calls take.
    //
    // LATCHED, and this is not an optimisation -- it is the difference between a read and a
    // full-array scan. `FindClassDefaultObject` goes to `FindObject`, which has NO cache and
    // walks `GUObjectArray` from index 0 rendering EVERY object's name to compare it; each
    // render allocates and frees an engine buffer. `FindClass` was given a cache on
    // 2026-08-25 for exactly this reason and `FindObject` never was. This function is called
    // per ROW per moving frame by the browser's hover pass, so an unlatched resolve here is
    // a per-frame full-array scan on a per-frame observer -- the precise shape that cost
    // this project 120 -> 60 fps once already.
    //
    // A CDO never moves for the life of the process, so one resolve is all there ever is.
    // A DYNAMIC-INITIALISER STATIC, so the resolve runs exactly ONCE -- including when it
    // FAILS. Written first as `if (!sLib) sLib = ...`, which latches only success: a null
    // resolve re-walked on every call, and this is called once per row per moving frame, so
    // the failure path WAS the per-frame full-array scan the latch was added to remove --
    // with an unthrottled UE_LOGE beside it, and log.cpp fflushes every non-INFO line. The
    // fix for a slow path must not be a fast path with a slow failure mode.
    static void* const sLib = [] { return R::FindClassDefaultObject(L"SlateBlueprintLibrary"); }();
    void* lib = sLib;
    if (!lib) {
        UE_LOGE("umg: SlateBlueprintLibrary has no CDO -- WidgetScreenRect unavailable");
        return false;
    }

    // How many bytes an FGeometry occupies, asked of the engine rather than declared here.
    // In LocalToAbsolute's frame the geometry is the first parameter, so the offset of the
    // one after it IS the padded size of the struct -- and GetCachedGeometry's frame holds
    // nothing but the returned geometry at offset 0. So the struct crosses from one frame
    // to the other as an opaque span, and this file never learns a single field of it.
    ParamFrame abs(absFn);
    const int32_t geomBytes = abs.ParamOffset(L"LocalCoordinate");
    if (geomBytes <= 0) {
        UE_LOGE("umg: LocalToAbsolute has no 'LocalCoordinate' parameter (offset=%d) -- the "
                "signature is not what this code was written against", geomBytes);
        return false;
    }

    ParamFrame geom(geomFn);
    if (geom.FrameSize() < geomBytes) {
        UE_LOGE("umg: GetCachedGeometry's frame is %d bytes but an FGeometry parameter is %d "
                "-- refusing to read past the frame", geom.FrameSize(), geomBytes);
        return false;
    }
    if (!Call(widget, geom)) return false;

    std::vector<uint8_t> blob(static_cast<size_t>(geomBytes), 0);
    if (!geom.GetRaw(L"ReturnValue", blob.data(), geomBytes)) return false;

    // LOCAL SIZE FIRST, because it is what makes the rect a rect. GetDesiredSize answers a
    // different question -- what the widget ASKED for, not what its parent gave it -- and a
    // button in a fill-weighted row is exactly where those two part company.
    ParamFrame size(sizeFn);
    // CROSS-CHECK, and it is the only guard against the silent corruption. Two drift modes
    // are already caught above (LocalCoordinate first -> geomBytes <= 0; geomBytes wider
    // than the geometry frame). The third is geomBytes too SMALL -- a parameter inserted
    // before Geometry, or Geometry not first -- and that one does not fail: the blob is a
    // truncated prefix, the tail of the struct stays zero, and this returns TRUE with a
    // plausible wrong rect, which downstream means clicking the wrong row. GetLocalSize
    // returns an FVector2D, so ITS ReturnValue offset must also be the padded size of the
    // geometry parameter. Two independent frames agreeing is a signature check; one frame's
    // offset is an assumption.
    if (size.ParamOffset(L"Geometry") != 0 ||
        size.ParamOffset(L"ReturnValue") != geomBytes) {
        UE_LOGE("umg: SlateBlueprintLibrary signature drift -- LocalToAbsolute puts an "
                "FGeometry at %d bytes but GetLocalSize disagrees (Geometry@%d, "
                "ReturnValue@%d). Refusing rather than reading a truncated struct.",
                geomBytes, size.ParamOffset(L"Geometry"), size.ParamOffset(L"ReturnValue"));
        return false;
    }
    if (!size.SetRaw(L"Geometry", blob.data(), geomBytes)) return false;
    if (!Call(lib, size)) return false;

    if (!abs.SetRaw(L"Geometry", blob.data(), geomBytes)) return false;
    abs.Set<FVector2D>(L"LocalCoordinate", FVector2D{0.f, 0.f});
    if (!Call(lib, abs)) return false;
    // BOTH WRITES AFTER THE LAST FAILURE POINT. The header promises the outs are untouched
    // when this returns false, and writing outSize before the second call broke that -- a
    // caller that logs a rect it was told not to trust prints a half-updated one.
    outSize    = size.Get<FVector2D>(L"ReturnValue");
    outTopLeft = abs.Get<FVector2D>(L"ReturnValue");
    return true;
}

bool ViewOffsetFraction(void* scrollBox, float& out) {
    void* fn = Resolve(g_scrollFrac);
    if (!scrollBox || !fn) return false;
    ParamFrame f(fn);
    if (!Call(scrollBox, f)) return false;
    out = f.Get<float>(L"ReturnValue");
    return true;
}

int32_t SwitcherIndex(void* switcher) {
    void* fn = Resolve(g_swGetIdx);
    if (!switcher || !fn) return -1;
    ParamFrame f(fn);
    if (!Call(switcher, f)) return -1;
    return f.Get<int32_t>(L"ReturnValue");
}

void ZeroBrushHandles(void* styleBase, const size_t* brushOffsets, int brushCount) {
    if (!styleBase || !brushOffsets) return;
    auto* base = reinterpret_cast<uint8_t*>(styleBase);
    for (int i = 0; i < brushCount; ++i)
        std::memset(base + brushOffsets[i] + P::off::FSlateBrush_ResourceHandle, 0,
                    P::off::FSlateBrush_HandleSize);
}

bool CloneStyle(void* dst, size_t dstOff, void* src, size_t srcOff, size_t styleSize,
                const size_t* brushOffsets, int brushCount) {
    if (!dst || !src || styleSize == 0) return false;
    auto* d = reinterpret_cast<uint8_t*>(dst) + dstOff;
    auto* s = reinterpret_cast<uint8_t*>(src) + srcOff;
    std::memcpy(d, s, styleSize);
    ZeroBrushHandles(d, brushOffsets, brushCount);
    return true;
}

bool SetImageTint(void* image, const FLinearColor& tint) {
    void* fn = Resolve(g_imgTint);
    if (!image || !fn) return false;
    // FSlateColor (0x28): SpecifiedColor (FLinearColor) @0x00, ColorUseRule @0x10 with
    // 0 = UseColor_Specified. Same shape SetTextBlockColorDispatch already builds.
    uint8_t sc[0x28] = {};
    std::memcpy(sc, &tint, sizeof(FLinearColor));
    ParamFrame f(fn);
    f.SetRaw(L"TintColor", sc, sizeof(sc));
    return Call(image, f);
}

bool SetImageTintRaw(void* image, const FLinearColor& tint) {
    if (!image) return false;
    auto* b = reinterpret_cast<uint8_t*>(image) + P::off::UImage_Brush;
    *reinterpret_cast<FLinearColor*>(b + P::off::FSlateBrush_TintColor) = tint;
    *(b + P::off::FSlateBrush_TintColor + P::off::FSlateColor_ColorUseRule) = 0;
    return true;
}

bool SetSizeBoxHeight(void* sizeBox, float height) {
    void* fn = Resolve(g_sbHeight);
    if (!sizeBox || !fn) return false;
    ParamFrame f(fn);
    f.Set<float>(L"InHeightOverride", height);
    return Call(sizeBox, f);
}

bool SetSizeBoxWidth(void* sizeBox, float width) {
    void* fn = Resolve(g_sbWidth);
    if (!sizeBox || !fn) return false;
    ParamFrame f(fn);
    f.Set<float>(L"InWidthOverride", width);
    return Call(sizeBox, f);
}

bool SetClipping(void* widget, uint8_t clipping) {
    void* fn = Resolve(g_setClip);
    if (!widget || !fn) return false;
    ParamFrame f(fn);
    f.Set<uint8_t>(L"InClipping", clipping);
    return Call(widget, f);
}

bool StyleTextBlock(void* textBlock, int32_t fontSize, const FLinearColor& color,
                    uint8_t justify) {
    if (!textBlock) return false;
    // BOUNDED RETRY, not a plain `if (!sFont)` and not a hard once-latch, because a FONT is
    // neither a CDO nor a UFunction: it is an ASSET, and an asset can be absent on one call
    // and present on a later one. So the negative cannot be latched forever the way `sLib`'s
    // is -- but it must not be retried freely either. `R::FindObject` is an uncached walk of
    // the whole GUObjectArray that RENDERS every object's name to compare it, and this
    // function runs FIVE TIMES PER ROW through AddText, so an unbounded retry on a 64-row
    // list is ~320 full walks in a single frame. Sixteen attempts spans several frames of
    // widget-building -- long enough for a late-loading font, short enough to be free -- and
    // then says so once instead of paying that cost forever.
    static void* sFont = nullptr;
    static int   sFontTries = 0;
    if (!sFont && sFontTries < 16) {
        sFont = R::FindObject(P::name::MenuFontName, P::name::FontClassName);
        if (!sFont && ++sFontTries == 16)
            UE_LOGW("umg: font '%ls' not found after %d attempts -- text blocks keep whatever "
                    "face they already carry, and this stops looking (the lookup is a full "
                    "object-array walk and runs 5x per row)",
                    P::name::MenuFontName, sFontTries);
    }
    auto* d = reinterpret_cast<uint8_t*>(textBlock);
    auto* font = d + P::off::UTextBlock_Font;
    // font_ui is loaded whenever the menu is up; if it somehow is not, leave whatever the
    // block already has rather than falling back to a different face -- a wrong font is a
    // visible defect, a default one is a silent one.
    if (sFont) *reinterpret_cast<void**>(font) = sFont;
    *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_Size) = fontSize;
    *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_OutlineSettings +
                                P::off::FFontOutlineSettings_OutlineSize) = 0;
    *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ColorAndOpacity) = color;
    *(d + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    *(d + P::off::UTextLayoutWidget_Justification) = justify;
    *reinterpret_cast<FVector2D*>(d + P::off::UTextBlock_ShadowOffset) = FVector2D{2.f, 2.f};
    *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ShadowColorAndOpacity) =
        FLinearColor{0.f, 0.f, 0.f, 1.f};
    return true;
}

bool SetSlotAlign(void* slot, size_t hAlignOff, size_t vAlignOff, uint8_t h, uint8_t v) {
    if (!slot) return false;
    auto* s = reinterpret_cast<uint8_t*>(slot);
    *(s + hAlignOff) = h;
    *(s + vAlignOff) = v;
    return true;
}

}  // namespace ue_wrap::umg
