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

// Resolved once per class and function pair. Classes and UFunctions never move within a
// process, so a null after a successful resolve is impossible and a null before one is a
// recook problem, which is why every miss below logs rather than failing silently.
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

// The panel widget base owns the whole generic panel API; one resolve serves every panel type.
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

// The content setter, the one owner, latched: the function lookup has no result cache and
// walks the whole object array, and callers sit inside the row build, once per row, so an
// unlatched resolve is dozens of full array walks in a single frame (measured at tens of
// milliseconds against a single-digit frame budget).
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

// The parameter name is the new-offset name from the header dump: the frame resolves by name
// off the live property chain, so a wrong name is a silent no-op write into a zeroed frame, a
// scroll to zero whatever was asked.
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
    // The button style embeds four Slate brushes, each with an unreflected resource handle; the
    // clone zeroes all four.
    CloneStyle(d, P::off::UButton_WidgetStyle, s, P::off::UButton_WidgetStyle,
               P::off::FButtonStyle_Size, P::off::FButtonStyleBrushes, 4);
    // Keep each Slate sound's resource object, so the button plays the native press and hover
    // sounds; zero only the trailing shared-pointer cache.
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
    // The visibility enum, and the two that matter are neighbours, which is how this class of bug
    // hides: hit-test-invisible takes the whole subtree out of the hit grid, and
    // self-hit-test-invisible only the widget itself, what a container wants. A chain that paints
    // correctly tells nothing about which one it carries.
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

// The cursor, in the same space the rects are in, asked of Slate rather than derived. The
// rects come from the local-to-absolute call, and their relationship to the OS cursor involves
// the window's client origin and the viewport's UI scale, which this file must not assume; the
// screen-to-absolute call is Slate's own inverse of the transform that produced the rects, so
// both sides of the comparison come from one source. The position is viewport pixels
// (screen-to-client first); the window position is excluded because the rects compared
// against are the plain local-to-absolute ones. False if the function or the context is
// unavailable, leaving `out` untouched, so the caller decides what a missing conversion means.
bool CursorToWidgetAbsolute(const FVector2D& screenPos, FVector2D& out) {
    void* fn = Resolve(g_screenToAbs);
    if (!fn) return false;
    static void* const sLib = [] { return R::FindClassDefaultObject(L"SlateBlueprintLibrary"); }();
    if (!sLib) return false;
    void* ctx = ue_wrap::engine::GetWorldContext();
    if (!ctx) return false;

    ParamFrame f(fn);
    // Named, never positional: a signature change must fail loudly rather than write a bool into
    // a float pair.
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

    // The library's functions are static, so they dispatch on the class default object, as the
    // engine's own function-library calls do. Latched, and that is the difference between a read
    // and a full-array scan: the default-object lookup is the uncached object find, which walks
    // the whole array rendering every object's name, and this is called per row per moving frame
    // by the browser's hover pass. A default object never moves for the life of the process, so
    // one resolve is all there is; a dynamic-initialiser static runs it exactly once, including
    // when it fails, where a latch of success only would re-walk on every call with an
    // unthrottled error log beside it (which the log flushes on every non-info line).
    static void* const sLib = [] { return R::FindClassDefaultObject(L"SlateBlueprintLibrary"); }();
    void* lib = sLib;
    if (!lib) {
        UE_LOGE("umg: SlateBlueprintLibrary has no CDO -- WidgetScreenRect unavailable");
        return false;
    }

    // How many bytes a geometry occupies, asked of the engine rather than declared here: in the
    // local-to-absolute frame the geometry is the first parameter, so the offset of the one after
    // it is the padded size of the struct, and the cached-geometry frame holds nothing but the
    // returned geometry at offset 0. The struct crosses from one frame to the other as an opaque
    // span, and this file never learns a single field of it.
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

    // The local size first, because it is what makes the rect a rect. The desired size answers a
    // different question, what the widget asked for rather than what its parent gave it, and a
    // button in a fill-weighted row is exactly where those two part company.
    ParamFrame size(sizeFn);
    // The cross-check, the only guard against silent corruption. Two drift modes are caught above
    // (the coordinate parameter first, the geometry wider than its frame); the third is the
    // geometry too small, a parameter inserted before it, and that one does not fail: the blob
    // is a truncated prefix, the tail stays zero, and this returns true with a plausible wrong
    // rect, which downstream means clicking the wrong row. The size call returns a vector, so its
    // return offset must also be the padded size of the geometry parameter; two independent
    // frames agreeing is a signature check, one frame's offset an assumption.
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
    // Both writes after the last failure point. The header promises the outs are untouched on
    // false, and writing the size before the second call broke that: a caller that logs a rect it
    // was told not to trust printed a half-updated one.
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
    // The Slate colour struct: the specified colour at the start and the use rule after it, 0
    // meaning use the specified colour; the same shape the text-block colour dispatch builds.
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
    // A bounded retry, neither a plain null check nor a hard once-latch: a font is an asset,
    // absent on one call and present on a later one, so the negative cannot be latched forever,
    // but the object find is an uncached walk of the whole object array rendering every name, and
    // this runs several times per row, so an unbounded retry on a long list is hundreds of full
    // walks in a frame. The attempts span several frames of widget building, long enough for a
    // late-loading font and short enough to be free, then it says so once.
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
    // The menu font is loaded whenever the menu is up; if it somehow is not, leave whatever the
    // block already has rather than fall back to a different face, since a wrong font is a
    // visible defect and a default one a silent one.
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

bool SetAutoWrapText(void* textBlock, bool wrap) {
    if (!textBlock) return false;
    static void* const sFn = [] {
        void* c = R::FindClass(P::name::TextBlockClass);
        return c ? R::FindFunction(c, L"SetAutoWrapText") : nullptr;
    }();
    if (!sFn) return false;
    ParamFrame f(sFn);
    f.Set<bool>(L"InAutoTextWrap", wrap);
    return Call(textBlock, f);
}

bool SetSlotHAlignLive(void* slot, uint8_t h) {
    if (!slot) return false;
    void* cls = R::ClassOf(slot);
    if (!cls) return false;
    // Cached per slot class, not globally: a screen mixes overlay slots and box slots, and each
    // declares its own setter. One entry is the steady state (the text field only ever asks about
    // overlay slots), so a small linear scan is the whole structure this needs, and it never grows
    // past the number of slot types in the tree.
    struct Entry { void* cls; void* fn; };
    static Entry sCache[8] = {};
    static int   sCount = 0;
    void* fn = nullptr;
    bool  known = false;
    for (int i = 0; i < sCount; ++i)
        if (sCache[i].cls == cls) { fn = sCache[i].fn; known = true; break; }
    if (!known) {
        fn = R::FindFunction(cls, L"SetHorizontalAlignment");
        if (sCount < 8) sCache[sCount++] = Entry{cls, fn};
        if (!fn)
            UE_LOGW("umg: %ls has no SetHorizontalAlignment -- a live alignment flip on it "
                    "will not take", R::ClassNameOf(slot).c_str());
    }
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<uint8_t>(L"InHorizontalAlignment", h);
    return Call(slot, f);
}

bool WidgetDesiredSize(void* widget, FVector2D& out) {
    if (!widget) return false;
    static void* const sFn = [] {
        void* w = R::FindClass(P::name::WidgetClass);
        return w ? R::FindFunction(w, L"GetDesiredSize") : nullptr;
    }();
    if (!sFn) return false;
    ParamFrame f(sFn);
    if (!Call(widget, f)) return false;
    out = f.Get<FVector2D>(L"ReturnValue");
    return true;
}

}  // namespace ue_wrap::umg
