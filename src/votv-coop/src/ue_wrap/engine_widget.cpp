// ue_wrap/engine_widget.cpp -- UMG widget construction (world + screen).
//
// Extracted from ue_wrap/engine.cpp (2026-05-25 modular refactor).
// Public API lives in ue_wrap/engine.h; this TU only implements the
// widget-related functions in `namespace ue_wrap::engine`.
//
// Builds:
//   - 3D world-space nameplate (UWidgetComponent on an Actor, translucent
//     UMG label) -- SpawnNameplateWidget
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
// SpawnNameplateWidget / SpawnScreenTextWidget call. File-private (the
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
    auto tU8 = reinterpret_cast<uint8_t*>(txt);
    if (g_npFont) *reinterpret_cast<void**>(tU8 + P::off::UTextBlock_Font) = g_npFont;
    *reinterpret_cast<int32_t*>(tU8 + P::off::UTextBlock_Font + P::off::FSlateFontInfo_Size) = fontSize;
    *reinterpret_cast<FLinearColor*>(tU8 + P::off::UTextBlock_ColorAndOpacity) = color;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextLayoutWidget_Justification) = justification;

    // Visual polish (2026-05-25, VT-comparison-driven): text outline + drop
    // shadow. These two writes close most of the "VoidTogether widgets look
    // natural / ours look bare" perception gap surfaced by the user. All three
    // surfaces benefit: nameplate (3D label), hud_feed (chat), pos_hud (dev HUD).
    //
    // FFontOutlineSettings lives at FSlateFontInfo + 0x10 (SlateCore.hpp:313);
    // OutlineSize @ +0x00 (int32, in pixels) + OutlineColor @ +0x10 (FLinearColor).
    // 1px outline + black gives a crisp glyph border that respects the text
    // alpha (FFontOutlineSettings has bSeparateFillAlpha @+0x04 default 0,
    // which means the outline shares the fill alpha -- if text is 0.22 alpha
    // for translucent nameplates, the outline is also 0.22 alpha, no
    // pop-out artifact).
    {
        const auto fontBase = tU8 + P::off::UTextBlock_Font;
        const auto outBase  = fontBase + P::off::FSlateFontInfo_OutlineSettings;
        *reinterpret_cast<int32_t*>(outBase + P::off::FFontOutlineSettings_OutlineSize)  = 1;
        FLinearColor outlineCol{0.f, 0.f, 0.f, 1.f};  // fully-opaque black; gated by fill alpha
        *reinterpret_cast<FLinearColor*>(outBase + P::off::FFontOutlineSettings_OutlineColor) = outlineCol;
    }
    // UTextBlock shadow: (1,1)px offset + black 50% alpha. UMG composites the
    // shadow as a duplicate text pass at the offset, alpha-multiplied by the
    // shadow color's alpha. Subtle drop-shadow look — like VoidTogether's
    // nameplate gets from its cooked widget's FSlateFontInfo settings.
    *reinterpret_cast<FVector2D*>(tU8 + P::off::UTextBlock_ShadowOffset) = FVector2D{1.f, 1.f};
    FLinearColor shadowCol{0.f, 0.f, 0.f, 0.5f};
    *reinterpret_cast<FLinearColor*>(tU8 + P::off::UTextBlock_ShadowColorAndOpacity) = shadowCol;

    SetTextOnBlock(txt, text);
    return {root, txt};
}

// Screen-space (viewport) widget functions, resolved on the UserWidget class.
void* g_addToVpFn = nullptr, *g_removeFromVpFn = nullptr, *g_widgetSetVisFn = nullptr;
void* g_setPosVpFn = nullptr, *g_setAlignVpFn = nullptr;
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

}  // namespace

void* SpawnNameplateWidget(const FVector& location, const wchar_t* text, float opacity,
                           void** outTextBlock) {
    if (outTextBlock) *outTextBlock = nullptr;
    if (!ResolveNameplateFns()) {
        UE_LOGE("engine: SpawnNameplateWidget unresolved (actor=%p comp=%p add=%p fin=%p spawnObj=%p uw=%p tb=%p)",
                g_npActorClass, g_npCompClass, g_npAddFn, g_npFinishFn, g_npSpawnObjFn, g_npUserWidgetClass, g_npTbClass);
        return nullptr;
    }
    void* actor = SpawnActor(g_npActorClass, location);
    if (!actor) { UE_LOGE("engine: SpawnNameplateWidget -- SpawnActor failed"); return nullptr; }

    uint8_t xform[0x30];
    MakeIdentityTransform(xform);

    // 1) AddComponentByClass(WidgetComponent, deferred) -- own widget, NOT a cooked one.
    void* comp = nullptr;
    {
        ParamFrame f(g_npAddFn);
        f.Set<void*>(L"Class", g_npCompClass);
        f.Set<bool>(L"bManualAttachment", false);
        f.SetRaw(L"relativeTransform", xform, sizeof(xform));
        f.Set<bool>(L"bDeferredFinish", true);
        if (!Call(actor, f)) { UE_LOGE("engine: SpawnNameplateWidget -- AddComponentByClass call failed"); return actor; }
        comp = f.Get<void*>(L"ReturnValue");
    }
    if (!comp) { UE_LOGE("engine: SpawnNameplateWidget -- no WidgetComponent returned"); return actor; }
    auto cU8 = reinterpret_cast<uint8_t*>(comp);

    // 2) BEFORE register: BlendMode=Transparent(2) + two-sided + draw-at-desired-size.
    // (IDA: GetMaterial(0) routes by BlendMode; the ctor defaults Masked(1), which
    // alpha-clips the content -> invisible. Transparent routes to TranslucentMaterial.)
    // Leave WidgetClass NULL -- we build + SetWidget our OWN UMG (no cooked widget).
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_BlendMode) = 2;
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bIsTwoSided) = 1;
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bDrawAtDesiredSize) = 1;

    // 3) FinishAddComponent -> register.
    {
        ParamFrame f(g_npFinishFn);
        f.Set<void*>(L"Component", comp);
        f.Set<bool>(L"bManualAttachment", false);
        f.SetRaw(L"relativeTransform", xform, sizeof(xform));
        Call(actor, f);
    }
    // Component draw config: auto-redraw, the translucent material slot, full tint.
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bManuallyRedraw) = 0;
    *reinterpret_cast<float*>(cU8 + P::off::UWidgetComponent_RedrawTime) = 0.f;
    if (g_npTransMat) *reinterpret_cast<void**>(cU8 + P::off::UWidgetComponent_TranslucentMaterial) = g_npTransMat;
    if (g_npTransMatOneSided) *reinterpret_cast<void**>(cU8 + P::off::UWidgetComponent_TranslucentMaterialOneSided) = g_npTransMatOneSided;
    if (g_npTintFn) { ParamFrame f(g_npTintFn); FLinearColor c{1.f, 1.f, 1.f, 1.f}; f.SetRaw(L"NewTintColorAndOpacity", &c, sizeof(c)); Call(comp, f); }

    // 4) Build OUR widget tree (shared builder): UUserWidget -> UWidgetTree ->
    // UTextBlock(root), translucent-white centred text at font size 56.
    // Font 56 + actor scale 0.15 (set in step 6) shrinks the world quad to
    // ~60% of the original font-14 / scale-1 baseline, with the SAME RT
    // pixel count as font 56 / scale 0.25 (since scale doesn't touch the
    // RT). Net: small visible nameplate, very high texel density.
    // Progression across retests:
    //   font 14, scale 1.00 -> baseline (pixelated; user complaint #1)
    //   font 28, scale 1.00 -> 2x density but 2x physical size ("what is this nameplate" -- complaint #2)
    //   font 28, scale 0.50 -> 2x density, baseline physical size (still "needs more resolution")
    //   font 56, scale 0.25 -> 4x density, baseline physical size
    //   font 56, scale 0.15 -> 4x density, ~60% baseline size (user: "make the scale smaller", current)
    // bDrawAtDesiredSize=1 still couples RT pixels to widget desired-size, so
    // a larger font yields a larger RT; the actor scale shrinks the visible
    // world quad without touching the RT pixel count.
    BuiltText bt = BuildTextWidget(actor, text, FLinearColor{1.f, 1.f, 1.f, opacity}, 56, /*Center*/ 1);
    void* root = bt.root;
    void* txt = bt.txt;
    if (!root || !txt) { UE_LOGE("engine: SpawnNameplateWidget -- BuildTextWidget failed"); return actor; }

    // 5) Attach our widget (re-parents + rebuilds the slate/render-target), then make
    // the runtime-added component tick so it actually draws its RT.
    if (g_npSetWidgetFn) { ParamFrame f(g_npSetWidgetFn); f.Set<void*>(L"Widget", root); Call(comp, f); }
    if (g_npTickFn) { ParamFrame f(g_npTickFn); f.Set<bool>(L"bEnabled", true); Call(comp, f); }
    if (g_npRenderUpdateFn) { ParamFrame f(g_npRenderUpdateFn); Call(comp, f); }
    if (g_npRedrawFn) { ParamFrame f(g_npRedrawFn); Call(comp, f); }

    // 6) Shrink the actor's world scale to make the visible quad small while
    // keeping the WidgetComponent's render-target pixel dimensions intact
    // (see step 4 comment for the RT-density-vs-world-size decoupling).
    // 0.15 -> nameplate quad is ~60% of the original font-14 / scale-1
    // baseline (user feedback "make the scale smaller"); font-56 RT
    // renders into that shrunken area at very high texel density.
    SetActorScale3D(actor, FVector{0.15f, 0.15f, 0.15f});

    UE_LOGI("engine: SpawnNameplateWidget(own) '%ls' actor=%p comp=%p root=%p txt=%p font=%p at (%.0f,%.0f,%.0f)",
            text, actor, comp, root, txt, g_npFont, location.X, location.Y, location.Z);
    if (outTextBlock) *outTextBlock = txt;
    return actor;
}

bool SetWidgetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !ResolveNameplateFns()) return false;
    SetTextOnBlock(textBlock, text);
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

}  // namespace ue_wrap::engine
