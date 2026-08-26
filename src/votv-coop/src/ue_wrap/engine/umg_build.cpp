// ue_wrap/engine/umg_build.cpp -- see ue_wrap/engine/umg_build.h.

#include "ue_wrap/engine/umg_build.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstring>

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

}  // namespace

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
    static void* sFont = nullptr;
    if (!sFont) sFont = R::FindObject(P::name::MenuFontName, P::name::FontClassName);
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
