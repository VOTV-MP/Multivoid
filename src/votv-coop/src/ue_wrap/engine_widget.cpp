// ue_wrap/engine_widget.cpp -- UMG widget construction (world + screen).
//
// Extracted from ue_wrap/engine.cpp (2026-05-25 modular refactor).
// Public API lives in ue_wrap/engine.h; this TU only implements the
// widget-related functions in `namespace ue_wrap::engine`.
//
// Builds:
//   - Screen-space HUD widgets (UUserWidget added to viewport) --
//     SpawnScreenTextWidget + AddWidgetToViewport + RemoveWidgetFromViewport
//   - Shared text-block construction with outline + drop-shadow polish
//   - SetWidgetText / SetTextOnBlock helpers

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// Identity FTransform (0x30): FQuat{0,0,0,1} @0x00, FVector Translation{0} @0x10,
// FVector Scale3D{1,1,1} @0x20.
void MakeIdentityTransform(uint8_t (&xform)[0x30]) {
    std::memset(xform, 0, sizeof(xform));
    float* f = reinterpret_cast<float*>(xform);
    f[3] = 1.f;                       // Quat.W
    f[8] = 1.f; f[9] = 1.f; f[10] = 1.f;  // Scale3D
}

// Cached UClasses + UFunctions for the widget pipeline. Resolved on first
// SpawnScreenTextWidget call. File-private (the
// other engine_* TUs have their own independent caches; sharing across
// files would require globals in a header).
void* g_npActorClass = nullptr, *g_npCompClass = nullptr;
void* g_npAddFn = nullptr, *g_npFinishFn = nullptr, *g_npTintFn = nullptr;
void* g_npRedrawFn = nullptr, *g_npRenderUpdateFn = nullptr, *g_npTickFn = nullptr, *g_npSetWidgetFn = nullptr;
void* g_npTransMat = nullptr, *g_npTransMatOneSided = nullptr;
void* g_npKtlCdo = nullptr, *g_npConvFn = nullptr;
// Own-widget construction (NewObject = UGameplayStatics::SpawnObject).
void* g_npGsCdo = nullptr, *g_npSpawnObjFn = nullptr;
void* g_npUserWidgetClass = nullptr, *g_npWidgetTreeClass = nullptr, *g_npTbClass = nullptr;
void* g_npTbSetTextFn = nullptr, *g_npFont = nullptr;
// Two-line nameplate (nick + separately-coloured bar): UVerticalBox root.
void* g_npVBoxClass = nullptr, *g_npAddChildVBoxFn = nullptr;

bool ResolveNameplateFns() {
    if (!g_npActorClass) g_npActorClass = R::FindClass(P::name::ActorClassName);
    if (!g_npCompClass) g_npCompClass = R::FindClass(P::name::WidgetComponentClass);
    if (g_npActorClass && !g_npAddFn) g_npAddFn = R::FindFunction(g_npActorClass, P::name::AddComponentByClassFn);
    if (g_npActorClass && !g_npFinishFn) g_npFinishFn = R::FindFunction(g_npActorClass, P::name::FinishAddComponentFn);
    if (g_npCompClass) {
        if (!g_npTintFn) g_npTintFn = R::FindFunction(g_npCompClass, P::name::SetTintColorAndOpacityFn);
        if (!g_npSetWidgetFn) g_npSetWidgetFn = R::FindFunction(g_npCompClass, P::name::SetWidgetFn);
        if (!g_npRedrawFn) g_npRedrawFn = R::FindFunction(g_npCompClass, P::name::RequestRedrawFn);
        if (!g_npRenderUpdateFn) g_npRenderUpdateFn = R::FindFunction(g_npCompClass, P::name::RequestRenderUpdateFn);
    }
    if (void* acc = R::FindClass(P::name::ActorComponentClass)) {
        if (!g_npTickFn) g_npTickFn = R::FindFunction(acc, P::name::SetComponentTickEnabledFn);
    }
    if (!g_npTransMat)
        g_npTransMat = R::FindObject(P::name::Widget3DTranslucentMatName, P::name::MaterialInstanceConstantClass);
    if (!g_npTransMatOneSided)
        g_npTransMatOneSided = R::FindObject(P::name::Widget3DTranslucentOneSidedMatName, P::name::MaterialInstanceConstantClass);
    // NewObject path + UMG classes + font.
    if (!g_npGsCdo) g_npGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_npGsCdo && !g_npSpawnObjFn) {
        if (void* c = R::ClassOf(g_npGsCdo)) g_npSpawnObjFn = R::FindFunction(c, P::name::SpawnObjectFn);
    }
    if (!g_npUserWidgetClass) g_npUserWidgetClass = R::FindClass(P::name::UserWidgetClass);
    if (!g_npWidgetTreeClass) g_npWidgetTreeClass = R::FindClass(P::name::WidgetTreeClass);
    if (!g_npTbClass) g_npTbClass = R::FindClass(P::name::TextBlockClass);
    if (g_npTbClass && !g_npTbSetTextFn) g_npTbSetTextFn = R::FindFunction(g_npTbClass, P::name::NameplateSetTextFn);  // UTextBlock::SetText(FText)
    // Two-line nameplate root (optional -- falls back to a single block if absent).
    if (!g_npVBoxClass) g_npVBoxClass = R::FindClass(P::name::VerticalBoxClass);
    if (g_npVBoxClass && !g_npAddChildVBoxFn)
        g_npAddChildVBoxFn = R::FindFunction(g_npVBoxClass, P::name::AddChildToVerticalBoxFn);
    if (!g_npFont) g_npFont = R::FindObject(P::name::FontName, P::name::FontClassName);
    if (!g_npKtlCdo) g_npKtlCdo = R::FindClassDefaultObject(P::name::KismetTextLibraryClass);
    if (g_npKtlCdo && !g_npConvFn) {
        if (void* c = R::ClassOf(g_npKtlCdo)) g_npConvFn = R::FindFunction(c, P::name::ConvStringToTextFn);
    }
    return g_npActorClass && g_npCompClass && g_npAddFn && g_npFinishFn &&
           g_npGsCdo && g_npSpawnObjFn && g_npUserWidgetClass && g_npWidgetTreeClass && g_npTbClass;
}

// NewObject by class via the reflected UGameplayStatics::SpawnObject(objectClass, Outer).
void* SpawnObject(void* objectClass, void* outer) {
    if (!g_npGsCdo || !g_npSpawnObjFn || !objectClass || !outer) return nullptr;
    ParamFrame f(g_npSpawnObjFn);
    f.Set<void*>(L"objectClass", objectClass);
    f.Set<void*>(L"Outer", outer);
    if (!Call(g_npGsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// Set a UTextBlock's text via Conv_StringToText -> UTextBlock::SetText. Requires
// ResolveNameplateFns() to have run (resolves g_npConvFn/g_npTbSetTextFn/g_npKtlCdo).
void SetTextOnBlock(void* txt, const wchar_t* text) {
    if (!txt || !g_npConvFn || !g_npTbSetTextFn || !g_npKtlCdo) return;
    uint8_t ftext[0x18] = {};
    std::wstring b(text);
    R::FString fs{b.data(), static_cast<int32_t>(b.size()) + 1, static_cast<int32_t>(b.size()) + 1};
    { ParamFrame cf(g_npConvFn); cf.SetRaw(L"inString", &fs, sizeof(fs)); Call(g_npKtlCdo, cf);  // 'inString' (lowercase i)
      cf.GetRaw(L"ReturnValue", ftext, sizeof(ftext)); }
    ParamFrame tf(g_npTbSetTextFn); tf.SetRaw(L"InText", ftext, sizeof(ftext)); Call(txt, tf);
}

// Build UUserWidget(outer) -> UWidgetTree -> UTextBlock(root) with the given font
// size / colour / justification and initial text. Shared by the world-space
// nameplate and the screen-space HUD feed (RULE 2: one UMG-text-build, not two).
// Returns {root, txt}; {nullptr,nullptr} on failure.
// Configure a freshly-spawned UTextBlock: font, size, colour (with the
// FSlateColor ColorUseRule forced to UseColor_Specified), 1px black outline,
// (1,1)px drop shadow, justification, and initial text. Shared by the
// single-block builder AND each block of the two-line nameplate (RULE 2: one
// text-block setup, not duplicated per builder).
//
// Visual polish (2026-05-25, VT-comparison-driven): the outline + shadow close
// most of the "VoidTogether widgets look natural / ours look bare" gap.
// FFontOutlineSettings lives at FSlateFontInfo + 0x10; OutlineSize @ +0x00
// (int32 px) + OutlineColor @ +0x10 (FLinearColor). bSeparateFillAlpha (+0x04,
// default 0) shares the fill alpha, so a 0.22-alpha nameplate gets a 0.22-alpha
// outline -- no pop-out artifact.
void ConfigureTextBlock(void* txt, const wchar_t* text, const FLinearColor& color,
                        int32_t fontSize, uint8_t justification) {
    auto tU8 = reinterpret_cast<uint8_t*>(txt);
    if (g_npFont) *reinterpret_cast<void**>(tU8 + P::off::UTextBlock_Font) = g_npFont;
    *reinterpret_cast<int32_t*>(tU8 + P::off::UTextBlock_Font + P::off::FSlateFontInfo_Size) = fontSize;
    *reinterpret_cast<FLinearColor*>(tU8 + P::off::UTextBlock_ColorAndOpacity) = color;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextLayoutWidget_Justification) = justification;
    {
        const auto fontBase = tU8 + P::off::UTextBlock_Font;
        const auto outBase  = fontBase + P::off::FSlateFontInfo_OutlineSettings;
        *reinterpret_cast<int32_t*>(outBase + P::off::FFontOutlineSettings_OutlineSize)  = 1;
        FLinearColor outlineCol{0.f, 0.f, 0.f, 1.f};  // fully-opaque black; gated by fill alpha
        *reinterpret_cast<FLinearColor*>(outBase + P::off::FFontOutlineSettings_OutlineColor) = outlineCol;
    }
    *reinterpret_cast<FVector2D*>(tU8 + P::off::UTextBlock_ShadowOffset) = FVector2D{1.f, 1.f};
    FLinearColor shadowCol{0.f, 0.f, 0.f, 0.5f};
    *reinterpret_cast<FLinearColor*>(tU8 + P::off::UTextBlock_ShadowColorAndOpacity) = shadowCol;
    SetTextOnBlock(txt, text);
}

// Build UUserWidget(outer) -> UWidgetTree -> UTextBlock(root). Single block,
// one colour. Used by the screen-space HUD feed (chat) + dev HUD.
struct BuiltText { void* root; void* txt; };
BuiltText BuildTextWidget(void* outer, const wchar_t* text, const FLinearColor& color,
                          int32_t fontSize, uint8_t justification) {
    void* root = SpawnObject(g_npUserWidgetClass, outer);
    void* tree = root ? SpawnObject(g_npWidgetTreeClass, root) : nullptr;
    void* txt  = tree ? SpawnObject(g_npTbClass, tree) : nullptr;
    if (!root || !tree || !txt) {
        UE_LOGE("engine: BuildTextWidget SpawnObject failed (root=%p tree=%p txt=%p)", root, tree, txt);
        return {nullptr, nullptr};
    }
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = txt;
    ConfigureTextBlock(txt, text, color, fontSize, justification);
    return {root, txt};
}

// Screen-space (viewport) widget functions, resolved on the UserWidget class.
void* g_addToVpFn = nullptr, *g_removeFromVpFn = nullptr, *g_widgetSetVisFn = nullptr;
void* g_setPosVpFn = nullptr, *g_setAlignVpFn = nullptr;
void* g_widgetSetOpacityFn = nullptr;  // UWidget::SetRenderOpacity (resolved lazily)
bool ResolveScreenWidgetFns() {
    if (!ResolveNameplateFns()) return false;  // UMG classes + SpawnObject + text fns
    if (g_npUserWidgetClass) {
        if (!g_addToVpFn) g_addToVpFn = R::FindFunction(g_npUserWidgetClass, P::name::AddToViewportFn);
        if (!g_removeFromVpFn) g_removeFromVpFn = R::FindFunction(g_npUserWidgetClass, P::name::RemoveFromViewportFn);
        if (!g_setPosVpFn) g_setPosVpFn = R::FindFunction(g_npUserWidgetClass, P::name::SetPositionInViewportFn);
        if (!g_setAlignVpFn) g_setAlignVpFn = R::FindFunction(g_npUserWidgetClass, P::name::SetAlignmentInViewportFn);
    }
    // SetVisibility is owned by UWidget (a parent), and FindFunction matches the
    // OWNING class only -- resolve it on "Widget", not "UserWidget".
    if (!g_widgetSetVisFn) {
        if (void* wc = R::FindClass(P::name::WidgetClass))
            g_widgetSetVisFn = R::FindFunction(wc, P::name::WidgetSetVisibilityFn);
    }
    return g_addToVpFn && g_widgetSetVisFn;
}

// ---- Runtime UMG button injection (MULTIPLAYER menu, P1) ----------------------
// UClasses + UFunctions for inserting a UButton into a UVerticalBox. Resolved lazily
// on the first InjectCanvasButton / WidgetIsHovered call (classes never move).
// (AddChildToVerticalBox itself is resolved by ResolveNameplateFns -> g_npAddChildVBoxFn.)
void* g_biButtonClass = nullptr, *g_biContentWidgetClass = nullptr;
void* g_biSetContentFn = nullptr, *g_biIsHoveredFn = nullptr;
void* g_biClearChildrenFn = nullptr;  // UPanelWidget::ClearChildren (insert-at-top reorder)
void* g_votvMenuFont = nullptr;       // font_ui (Share Tech Mono) -- VOTV's native menu label font

bool ResolveButtonInjectFns() {
    if (!ResolveNameplateFns()) return false;  // SpawnObject + UMG text classes + text fns + AddChildToVerticalBox
    // font_ui is the font VOTV's ui_menu labels use; resolve it so the injected
    // MULTIPLAYER label matches them (it is loaded whenever the menu is up).
    if (!g_votvMenuFont) g_votvMenuFont = R::FindObject(P::name::MenuFontName, P::name::FontClassName);
    if (!g_biButtonClass) g_biButtonClass = R::FindClass(P::name::ButtonClass);
    if (!g_biContentWidgetClass) g_biContentWidgetClass = R::FindClass(P::name::ContentWidgetClass);
    if (g_biContentWidgetClass && !g_biSetContentFn)
        g_biSetContentFn = R::FindFunction(g_biContentWidgetClass, P::name::SetContentFn);
    if (void* pwc = R::FindClass(P::name::PanelWidgetClass)) {
        if (!g_biClearChildrenFn) g_biClearChildrenFn = R::FindFunction(pwc, P::name::ClearChildrenFn);
    }
    // IsHovered + SetVisibility are owned by UWidget (FindFunction = owning class).
    if (void* wc = R::FindClass(P::name::WidgetClass)) {
        if (!g_biIsHoveredFn) g_biIsHoveredFn = R::FindFunction(wc, P::name::WidgetIsHoveredFn);
        if (!g_widgetSetVisFn) g_widgetSetVisFn = R::FindFunction(wc, P::name::WidgetSetVisibilityFn);
    }
    return g_biButtonClass && g_biSetContentFn && g_biIsHoveredFn;
}

}  // namespace

bool SetWidgetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !ResolveNameplateFns()) return false;
    SetTextOnBlock(textBlock, text);
    return true;
}

bool SetTextBlockColor(void* textBlock, const FLinearColor& color) {
    if (!textBlock) return false;
    // Same write BuildTextWidget uses at construction (lines above): the
    // FLinearColor at +ColorAndOpacity + the FSlateColor ColorUseRule byte = 0
    // (UseColor_Specified). The auto-redrawing UWidgetComponent picks it up next
    // frame; no UFunction dispatch needed.
    auto* base = reinterpret_cast<uint8_t*>(textBlock);
    *reinterpret_cast<FLinearColor*>(base + P::off::UTextBlock_ColorAndOpacity) = color;
    *reinterpret_cast<uint8_t*>(base + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    return true;
}

bool AddWidgetToViewport(void* userWidget, int zOrder) {
    if (!userWidget || !ResolveScreenWidgetFns() || !g_addToVpFn) return false;
    ParamFrame f(g_addToVpFn);
    f.Set<int32_t>(L"ZOrder", zOrder);
    return Call(userWidget, f);
}

bool RemoveWidgetFromViewport(void* userWidget) {
    if (!userWidget || !ResolveScreenWidgetFns() || !g_removeFromVpFn) return false;
    ParamFrame f(g_removeFromVpFn);
    return Call(userWidget, f);
}

bool SpawnScreenTextWidget(void* outer, int zOrder, FVector2D alignment, FVector2D position,
                           int justify, int fontSize, const FLinearColor& color,
                           void** outRoot, void** outText) {
    if (outRoot) *outRoot = nullptr;
    if (outText) *outText = nullptr;
    if (!outer || !ResolveScreenWidgetFns()) {
        UE_LOGE("engine: SpawnScreenTextWidget unresolved (uw=%p addVp=%p setVis=%p)",
                g_npUserWidgetClass, g_addToVpFn, g_widgetSetVisFn);
        return false;
    }
    // Multi-line text (caller drives via SetWidgetText). Outer should be a
    // persistent object (GameInstance) so the widget survives level loads.
    BuiltText bt = BuildTextWidget(outer, L"", color, fontSize, static_cast<uint8_t>(justify));
    if (!bt.root || !bt.txt) return false;
    // Visible but input-transparent (ESlateVisibility::HitTestInvisible = 3) so the
    // overlay never steals mouse/keyboard focus from the game.
    if (g_widgetSetVisFn) { ParamFrame f(g_widgetSetVisFn); f.Set<uint8_t>(L"InVisibility", 3); Call(bt.root, f); }
    if (!AddWidgetToViewport(bt.root, zOrder)) {
        UE_LOGE("engine: SpawnScreenTextWidget -- AddToViewport failed");
        return false;
    }
    // Alignment is the pivot inside the widget that gets placed at `position`:
    // {0,0} top-left, {1,0} top-right, {0,.5} left-middle, {0.5,0.5} centre, etc.
    // Position pixels assume 1920x1080; tune (or use viewport-relative math) for other res.
    if (g_setAlignVpFn) { ParamFrame f(g_setAlignVpFn); FVector2D a = alignment; f.SetRaw(L"Alignment", &a, sizeof(a)); Call(bt.root, f); }
    if (g_setPosVpFn)   { ParamFrame f(g_setPosVpFn);   FVector2D p = position;  f.SetRaw(L"Position",  &p, sizeof(p)); f.Set<bool>(L"bRemoveDPIScale", true); Call(bt.root, f); }
    if (outRoot) *outRoot = bt.root;
    if (outText) *outText = bt.txt;
    UE_LOGI("engine: SpawnScreenTextWidget root=%p txt=%p z=%d align=(%.1f,%.1f) pos=(%.0f,%.0f)",
            bt.root, bt.txt, zOrder, alignment.X, alignment.Y, position.X, position.Y);
    return true;
}

bool InjectCanvasButton(void* refButton, const wchar_t* label, void** outButton) {
    if (outButton) *outButton = nullptr;
    if (!refButton || !label) return false;
    if (!ResolveButtonInjectFns()) {
        UE_LOGE("engine: InjectCanvasButton unresolved (btnCls=%p setContentFn=%p hoverFn=%p "
                "clearChildrenFn=%p addVBoxFn=%p)",
                g_biButtonClass, g_biSetContentFn, g_biIsHoveredFn, g_biClearChildrenFn,
                g_npAddChildVBoxFn);
        return false;
    }

    // refButton (e.g. NEW GAME) lives in a list panel -- a UVerticalBox. We add our
    // button INTO that VerticalBox so it auto-positions exactly like the other menu
    // items (the VBox owns layout/spacing -- no fragile canvas-anchor math), then
    // reorder it to the TOP (above NEW GAME).
    void* refSlot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refButton) + P::off::UWidget_Slot);
    void* listBox = refSlot ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refSlot) + P::off::UPanelSlot_Parent) : nullptr;
    if (!listBox || !g_npAddChildVBoxFn) {
        UE_LOGE("engine: InjectCanvasButton -- no list box / AddChildToVerticalBox (refSlot=%p listBox=%p addVBox=%p)",
                refSlot, listBox, g_npAddChildVBoxFn);
        return false;
    }

    void* button = SpawnObject(g_biButtonClass, listBox);
    void* txt = button ? SpawnObject(g_npTbClass, button) : nullptr;
    if (!button || !txt) {
        UE_LOGE("engine: InjectCanvasButton SpawnObject failed (button=%p txt=%p)", button, txt);
        return false;
    }

    // Label styling: match VOTV's native menu items DETERMINISTICALLY. We do NOT clone
    // a reference UTextBlock (tex_btnStart): its pointer is null at some inject timings,
    // which fell through to a Roboto / centered / white default -- the "wrong font,
    // indented, wrong colour" bug. The native ui_menu labels are font_ui (Share Tech
    // Mono) @ size 16, left-justified, no outline, drop shadow offset (2,2) opaque black
    // (verified: bp_reflection/ui_menu.json tex_btnStart/tex_btnstat -> FontObject=font_ui,
    // TypefaceFontName="Font", Size=16, ShadowOffset=(2,2), ShadowColor=(0,0,0,1)). We set
    // exactly that here and tint the label CYAN to mark the coop entry. TypefaceFontName is
    // left None -> font_ui's single (default) typeface, correct for this one-face font.
    {
        auto* d = reinterpret_cast<uint8_t*>(txt);
        auto* font = d + P::off::UTextBlock_Font;
        // Prefer font_ui (the native menu font); fall back to engine Roboto so the label
        // is always readable if font_ui hasn't resolved. (Font parity is deferred.)
        void* chosenFont = g_votvMenuFont ? g_votvMenuFont : g_npFont;
        if (chosenFont) *reinterpret_cast<void**>(font) = chosenFont;
        *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_Size) = 16;
        // No outline (native has none) -- OutlineSize 0.
        *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_OutlineSettings +
                                    P::off::FFontOutlineSettings_OutlineSize) = 0;
        // Cyan, ColorUseRule = UseColor_Specified (0).
        *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ColorAndOpacity) =
            FLinearColor{0.f, 1.f, 1.f, 1.f};
        *(d + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
        // Left-justify (0), matching the native list items (fixes the indent).
        *(d + P::off::UTextLayoutWidget_Justification) = 0;
        // Drop shadow: offset (2,2), opaque black -- exactly the native menu labels.
        *reinterpret_cast<FVector2D*>(d + P::off::UTextBlock_ShadowOffset) = FVector2D{2.f, 2.f};
        *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ShadowColorAndOpacity) =
            FLinearColor{0.f, 0.f, 0.f, 1.f};
        SetTextOnBlock(txt, label);
    }

    // Nest the label inside the button (UContentWidget::SetContent).
    { ParamFrame f(g_biSetContentFn); f.Set<void*>(L"Content", txt); Call(button, f); }

    // Left-align the label within the button. SetContent created a UButtonSlot whose
    // default HAlign is Center -> the label sat INDENTED vs the flush-left native items.
    // Set it to Fill + zero padding so the text block spans the button and its Left text
    // justification pins the text to the button's left edge (= flush with NEW GAME etc.).
    // The slot is reachable via the label's UWidget::Slot back-pointer (set by SetContent).
    if (void* cslot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(txt) + P::off::UWidget_Slot)) {
        auto* cs = reinterpret_cast<uint8_t*>(cslot);
        *(cs + P::off::UButtonSlot_HAlign) = 0;  // HAlign_Fill
        *(cs + P::off::UButtonSlot_VAlign) = 2;  // VAlign_Center
        std::memset(cs + P::off::UButtonSlot_Padding, 0, 0x10);  // FMargin = 0 (no left indent)
    }

    // Clone the reference button's visual style (brushes + tint) so ours matches.
    if (refButton) {
        auto* d = reinterpret_cast<uint8_t*>(button);
        auto* s = reinterpret_cast<uint8_t*>(refButton);
        std::memcpy(d + P::off::UButton_WidgetStyle, s + P::off::UButton_WidgetStyle,
                    P::off::FButtonStyle_Size);
        // FButtonStyle's two FSlateSound members each hold an unreflected
        // TSharedPtr<FSlateSoundResource> cache past the ResourceObject; the raw memcpy
        // shallow-aliased that refcounted cache (no AddRef). KEEP the copied ResourceObject
        // (@ 0x00) so the button plays the native press + hover sounds -- the user pressing/
        // hovering our real UButton drives Slate, which plays them -- and zero ONLY the
        // trailing cache (SlateCore rebuilds it lazily from ResourceObject).
        std::memset(d + P::off::UButton_WidgetStyle + P::off::FButtonStyle_PressedSlateSound +
                    P::off::FSlateSound_CacheStart, 0, P::off::FSlateSound_Size - P::off::FSlateSound_CacheStart);
        std::memset(d + P::off::UButton_WidgetStyle + P::off::FButtonStyle_HoveredSlateSound +
                    P::off::FSlateSound_CacheStart, 0, P::off::FSlateSound_Size - P::off::FSlateSound_CacheStart);
        *reinterpret_cast<FLinearColor*>(d + P::off::UButton_ColorAndOpacity) =
            *reinterpret_cast<FLinearColor*>(s + P::off::UButton_ColorAndOpacity);
        *reinterpret_cast<FLinearColor*>(d + P::off::UButton_BackgroundColor) =
            *reinterpret_cast<FLinearColor*>(s + P::off::UButton_BackgroundColor);
    }

    // Insert at the TOP of the VerticalBox. UMG has no insert-at-index, so the
    // canonical reorder is: snapshot the current children, ClearChildren (DETACHES
    // them -- the widget OBJECTS survive, still referenced by ui_menu_C's fields), then
    // re-add OUR button first + the originals after. If the snapshot fails (or the list
    // is bigger than our buffer) we DELIBERATELY fall back to a plain append (bottom) so
    // we NEVER ClearChildren without being able to fully restore the menu.
    constexpr int kMaxList = 128;  // VOTV's menu VBox has ~13 children; generous headroom
    void* prev[kMaxList]; int prevN = 0;
    {
        auto* sp = reinterpret_cast<uint8_t*>(listBox) + P::off::UPanelWidget_Slots;
        void* data = *reinterpret_cast<void**>(sp);
        const int32_t n = *reinterpret_cast<int32_t*>(sp + 0x8);
        if (data && n > 0 && n <= kMaxList) {
            auto** slots = reinterpret_cast<void**>(data);
            for (int i = 0; i < n; ++i) {
                if (void* sl = slots[i])
                    prev[prevN++] = *reinterpret_cast<void**>(
                        reinterpret_cast<uint8_t*>(sl) + P::off::UPanelSlot_Content);
            }
        } else if (n > kMaxList) {
            UE_LOGW("engine: InjectCanvasButton -- VerticalBox has %d children (> cap %d); "
                    "appending at bottom instead of inserting at top (no ClearChildren)", n, kMaxList);
        }
    }
    auto addToVBox = [&](void* child) {
        ParamFrame f(g_npAddChildVBoxFn); f.Set<void*>(L"Content", child); Call(listBox, f);
    };
    if (prevN > 0 && g_biClearChildrenFn) {
        { ParamFrame cf(g_biClearChildrenFn); Call(listBox, cf); }
        addToVBox(button);                                   // MULTIPLAYER -> index 0 (top)
        for (int i = 0; i < prevN; ++i) addToVBox(prev[i]);  // originals re-added below
        UE_LOGI("engine: InjectCanvasButton inserted at TOP of VerticalBox (%d items re-added below)", prevN);
    } else {
        addToVBox(button);
        UE_LOGW("engine: InjectCanvasButton -- list snapshot empty (n=%d); appended at bottom", prevN);
    }

    // Match the reference item's VBox SLOT layout (padding + H/V alignment + size) so
    // our button sits EXACTLY like NEW GAME -- same indent, same spacing. The slot was
    // created fresh by AddChildToVerticalBox with defaults; copy the reference's layout
    // region (excludes the base UPanelSlot Parent/Content ptrs, so no aliasing).
    {
        void* ourSlot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + P::off::UWidget_Slot);
        void* refSlot2 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refButton) + P::off::UWidget_Slot);
        if (ourSlot && refSlot2) {
            std::memcpy(reinterpret_cast<uint8_t*>(ourSlot) + P::off::UVerticalBoxSlot_LayoutStart,
                        reinterpret_cast<uint8_t*>(refSlot2) + P::off::UVerticalBoxSlot_LayoutStart,
                        P::off::UVerticalBoxSlot_LayoutSize);
        }
    }

    // Force Visible (input-receiving) so the hover/click poll sees it.
    if (g_widgetSetVisFn) {
        ParamFrame f(g_widgetSetVisFn); f.Set<uint8_t>(L"InVisibility", 0); Call(button, f);
    }

    if (outButton) *outButton = button;
    UE_LOGI("engine: InjectCanvasButton '%ls' button=%p listBox=%p", label, button, listBox);
    return true;
}

bool WidgetIsHovered(void* widget) {
    if (!widget || !ResolveButtonInjectFns() || !g_biIsHoveredFn) return false;
    ParamFrame f(g_biIsHoveredFn);
    if (!Call(widget, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool SetWidgetVisibility(void* widget, uint8_t slateVis) {
    // ResolveButtonInjectFns resolves g_widgetSetVisFn (UWidget::SetVisibility) as part of
    // the inject set; reuse it rather than a second resolver for one UFunction.
    if (!widget || !ResolveButtonInjectFns() || !g_widgetSetVisFn) return false;
    ParamFrame f(g_widgetSetVisFn);
    f.Set<uint8_t>(L"InVisibility", slateVis);
    return Call(widget, f);
}

bool SetWidgetRenderOpacity(void* widget, float opacity) {
    if (!widget) return false;
    if (!g_widgetSetOpacityFn) {
        if (void* wc = R::FindClass(P::name::WidgetClass))
            g_widgetSetOpacityFn = R::FindFunction(wc, P::name::WidgetSetRenderOpacityFn);
    }
    if (!g_widgetSetOpacityFn) return false;
    ParamFrame f(g_widgetSetOpacityFn);
    f.Set<float>(L"InOpacity", opacity);
    return Call(widget, f);
}

}  // namespace ue_wrap::engine
