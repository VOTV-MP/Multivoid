// ue_wrap/engine/engine_widget.cpp -- UMG widget construction: the screen-space text widget
// (a user widget added to the viewport), the shared text-block setup with outline and shadow,
// the text and colour setters, and the two menu injects (a button into a vertical box, a text
// row above a label). The public API is in ue_wrap/engine/engine.h.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/umg_build.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// The identity transform: the quaternion at 0, the translation at 0x10, the scale at 0x20.
void MakeIdentityTransform(uint8_t (&xform)[0x30]) {
    std::memset(xform, 0, sizeof(xform));
    float* f = reinterpret_cast<float*>(xform);
    f[3] = 1.f;                       // Quat.W
    f[8] = 1.f; f[9] = 1.f; f[10] = 1.f;  // Scale3D
}

// The cached classes and functions for the widget pipeline, resolved on first use; file-private,
// since the other engine TUs keep their own caches.
void* g_npActorClass = nullptr, *g_npCompClass = nullptr;
void* g_npAddFn = nullptr, *g_npFinishFn = nullptr, *g_npTintFn = nullptr;
void* g_npRedrawFn = nullptr, *g_npRenderUpdateFn = nullptr, *g_npTickFn = nullptr, *g_npSetWidgetFn = nullptr;
void* g_npTransMat = nullptr, *g_npTransMatOneSided = nullptr;
void* g_npKtlCdo = nullptr, *g_npConvFn = nullptr;
// Own-widget construction: NewObject through the gameplay statics' SpawnObject.
void* g_npGsCdo = nullptr, *g_npSpawnObjFn = nullptr;
void* g_npUserWidgetClass = nullptr, *g_npWidgetTreeClass = nullptr, *g_npTbClass = nullptr;
void* g_npTbSetTextFn = nullptr, *g_npFont = nullptr;
// The vertical box class and its add-child function.
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
    // The NewObject path, the UMG classes and the font.
    if (!g_npGsCdo) g_npGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_npGsCdo && !g_npSpawnObjFn) {
        if (void* c = R::ClassOf(g_npGsCdo)) g_npSpawnObjFn = R::FindFunction(c, P::name::SpawnObjectFn);
    }
    if (!g_npUserWidgetClass) g_npUserWidgetClass = R::FindClass(P::name::UserWidgetClass);
    if (!g_npWidgetTreeClass) g_npWidgetTreeClass = R::FindClass(P::name::WidgetTreeClass);
    if (!g_npTbClass) g_npTbClass = R::FindClass(P::name::TextBlockClass);
    if (g_npTbClass && !g_npTbSetTextFn) g_npTbSetTextFn = R::FindFunction(g_npTbClass, P::name::NameplateSetTextFn);  // UTextBlock::SetText(FText)
    // The vertical box, optional.
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

// NewObject by class, through the reflected SpawnObject.
void* SpawnObject(void* objectClass, void* outer) {
    if (!g_npGsCdo || !g_npSpawnObjFn || !objectClass || !outer) return nullptr;
    ParamFrame f(g_npSpawnObjFn);
    f.Set<void*>(L"objectClass", objectClass);
    f.Set<void*>(L"Outer", outer);
    if (!Call(g_npGsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// Set a text block's text through the string-to-text conversion and SetText; requires the
// resolve above.
void SetTextOnBlock(void* txt, const wchar_t* text) {
    if (!txt || !g_npConvFn || !g_npTbSetTextFn || !g_npKtlCdo) return;
    uint8_t ftext[0x18] = {};
    std::wstring b(text);
    R::FString fs{b.data(), static_cast<int32_t>(b.size()) + 1, static_cast<int32_t>(b.size()) + 1};
    { ParamFrame cf(g_npConvFn); cf.SetRaw(L"inString", &fs, sizeof(fs)); Call(g_npKtlCdo, cf);  // 'inString' (lowercase i)
      cf.GetRaw(L"ReturnValue", ftext, sizeof(ftext)); }
    ParamFrame tf(g_npTbSetTextFn); tf.SetRaw(L"InText", ftext, sizeof(ftext)); Call(txt, tf);
}

// Configure a freshly spawned text block: font, size, colour with the use rule forced to
// specified, a 1 px black outline, a (1,1) drop shadow, justification and the initial text.
// Shared by the single-block builder and every other block this TU builds. The outline
// colour shares the fill alpha, so a translucent plate gets a matching outline.
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

// A user widget with a widget tree and a text block as its root: a single block, one colour,
// for the screen-space feed and the dev HUD.
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

// The viewport widget functions, resolved on the user-widget class.
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
    // SetVisibility is owned by the widget base class, and FindFunction matches the owning class
    // only, so it resolves on Widget, not UserWidget.
    if (!g_widgetSetVisFn) {
        if (void* wc = R::FindClass(P::name::WidgetClass))
            g_widgetSetVisFn = R::FindFunction(wc, P::name::WidgetSetVisibilityFn);
    }
    return g_addToVpFn && g_widgetSetVisFn;
}

// The runtime button injection: the classes and functions for inserting a button into a
// vertical box, resolved lazily on the first inject or hover query.
void* g_biButtonClass = nullptr, *g_biContentWidgetClass = nullptr;
void* g_biSetContentFn = nullptr, *g_biIsHoveredFn = nullptr;
void* g_biClearChildrenFn = nullptr;  // UPanelWidget::ClearChildren (insert-at-top reorder)
void* g_votvMenuFont = nullptr;       // font_ui (Share Tech Mono) -- VOTV's native menu label font

bool ResolveButtonInjectFns() {
    if (!ResolveNameplateFns()) return false;  // SpawnObject + UMG text classes + text fns + AddChildToVerticalBox
    // The game's menu-label font, resolved so the injected label matches; loaded whenever the menu
    // is up.
    if (!g_votvMenuFont) g_votvMenuFont = R::FindObject(P::name::MenuFontName, P::name::FontClassName);
    if (!g_biButtonClass) g_biButtonClass = R::FindClass(P::name::ButtonClass);
    if (!g_biContentWidgetClass) g_biContentWidgetClass = R::FindClass(P::name::ContentWidgetClass);
    if (g_biContentWidgetClass && !g_biSetContentFn)
        g_biSetContentFn = R::FindFunction(g_biContentWidgetClass, P::name::SetContentFn);
    if (void* pwc = R::FindClass(P::name::PanelWidgetClass)) {
        if (!g_biClearChildrenFn) g_biClearChildrenFn = R::FindFunction(pwc, P::name::ClearChildrenFn);
    }
    // IsHovered and SetVisibility are owned by the widget base class.
    if (void* wc = R::FindClass(P::name::WidgetClass)) {
        if (!g_biIsHoveredFn) g_biIsHoveredFn = R::FindFunction(wc, P::name::WidgetIsHoveredFn);
        if (!g_widgetSetVisFn) g_widgetSetVisFn = R::FindFunction(wc, P::name::WidgetSetVisibilityFn);
    }
    return g_biButtonClass && g_biSetContentFn && g_biIsHoveredFn;
}

// Insert `child` at the top of a vertical box. UMG has no insert-at-index, so the reorder is:
// snapshot the children, clear (which detaches them; the objects survive, referenced by their
// owner's fields), then re-add the child first and the originals after. If the snapshot fails
// or the list exceeds the buffer, a plain append at the bottom, so the panel is never cleared
// without being fully restorable. Shared by the button inject and the text-row inject. Game
// thread.
bool InsertAtTopOfVBox(void* vbox, void* child) {
    if (!vbox || !child || !g_npAddChildVBoxFn) return false;
    constexpr int kMaxList = 128;  // VOTV's menu VBoxes have <= ~13 children; generous headroom
    // Each child is snapshotted with its slot layout region: the clear destroys the slots and the
    // re-add creates fresh default ones, so without the restore every native row's padding and
    // alignment would silently reset.
    void* prev[kMaxList]; uint8_t prevLayout[kMaxList][P::off::UVerticalBoxSlot_LayoutSize];
    int prevN = 0;
    {
        auto* sp = reinterpret_cast<uint8_t*>(vbox) + P::off::UPanelWidget_Slots;
        void* data = *reinterpret_cast<void**>(sp);
        const int32_t n = *reinterpret_cast<int32_t*>(sp + 0x8);
        if (data && n > 0 && n <= kMaxList) {
            auto** slots = reinterpret_cast<void**>(data);
            for (int i = 0; i < n; ++i) {
                if (void* sl = slots[i]) {
                    prev[prevN] = *reinterpret_cast<void**>(
                        reinterpret_cast<uint8_t*>(sl) + P::off::UPanelSlot_Content);
                    std::memcpy(prevLayout[prevN],
                                reinterpret_cast<uint8_t*>(sl) + P::off::UVerticalBoxSlot_LayoutStart,
                                P::off::UVerticalBoxSlot_LayoutSize);
                    ++prevN;
                }
            }
        } else if (n > kMaxList) {
            UE_LOGW("engine: InsertAtTopOfVBox -- VerticalBox has %d children (> cap %d); "
                    "appending at bottom instead (no ClearChildren)", n, kMaxList);
        }
    }
    auto addToVBox = [&](void* c) {
        ParamFrame f(g_npAddChildVBoxFn); f.Set<void*>(L"Content", c); Call(vbox, f);
    };
    // Restore a widget's saved layout onto its new slot, re-read through the widget's slot pointer;
    // the old slot is dead after the clear.
    auto restoreLayout = [&](void* w, const uint8_t* saved) {
        void* sl = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(w) + P::off::UWidget_Slot);
        if (sl)
            std::memcpy(reinterpret_cast<uint8_t*>(sl) + P::off::UVerticalBoxSlot_LayoutStart,
                        saved, P::off::UVerticalBoxSlot_LayoutSize);
    };
    if (prevN > 0 && g_biClearChildrenFn) {
        { ParamFrame cf(g_biClearChildrenFn); Call(vbox, cf); }
        addToVBox(child);                                    // ours -> index 0 (top)
        for (int i = 0; i < prevN; ++i) {                    // originals re-added below,
            addToVBox(prev[i]);                              // each with its layout restored
            restoreLayout(prev[i], prevLayout[i]);
        }
        UE_LOGI("engine: InsertAtTopOfVBox inserted at TOP (%d items re-added below, layouts restored)", prevN);
    } else {
        addToVBox(child);
        UE_LOGW("engine: InsertAtTopOfVBox -- list snapshot empty (n=%d); appended at bottom", prevN);
    }
    return true;
}

}  // namespace

void* SpawnUObject(void* objectClass, void* outer) {
    // The resolve above owns the gameplay-statics CDO and the SpawnObject function every widget
    // build goes through; reused rather than resolved twice.
    if (!ResolveNameplateFns()) return nullptr;
    return SpawnObject(objectClass, outer);
}

bool SetWidgetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !ResolveNameplateFns()) return false;
    SetTextOnBlock(textBlock, text);
    return true;
}

bool SetTextBlockColor(void* textBlock, const FLinearColor& color) {
    if (!textBlock) return false;
    // The same write the builder uses at construction: the colour plus the use rule byte set to
    // specified. The auto-redrawing widget component picks it up next frame with no dispatch.
    auto* base = reinterpret_cast<uint8_t*>(textBlock);
    *reinterpret_cast<FLinearColor*>(base + P::off::UTextBlock_ColorAndOpacity) = color;
    *reinterpret_cast<uint8_t*>(base + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    return true;
}

bool SetTextBlockColorDispatch(void* textBlock, const FLinearColor& color) {
    if (!textBlock || !ResolveNameplateFns() || !g_npTbClass) return false;
    static void* s_setColorFn = nullptr;  // UTextBlock::SetColorAndOpacity (never moves)
    if (!s_setColorFn) s_setColorFn = R::FindFunction(g_npTbClass, P::name::TextBlockSetColorFn);
    if (!s_setColorFn) return false;
    // The slate colour: the specified colour at 0, the use rule at 0x10 (0 is specified), the
    // trailing bytes zeroed.
    uint8_t sc[0x28] = {};
    std::memcpy(sc, &color, sizeof(FLinearColor));
    ParamFrame f(s_setColorFn);
    f.SetRaw(L"InColorAndOpacity", sc, sizeof(sc));
    return Call(textBlock, f);
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
    // Multi-line text the caller drives through SetWidgetText; the outer should be a persistent
    // object, so the widget survives level loads.
    BuiltText bt = BuildTextWidget(outer, L"", color, fontSize, static_cast<uint8_t>(justify));
    if (!bt.root || !bt.txt) return false;
    // Visible but input-transparent (hit-test-invisible is 3), so the overlay never steals focus
    // from the game.
    if (g_widgetSetVisFn) { ParamFrame f(g_widgetSetVisFn); f.Set<uint8_t>(L"InVisibility", 3); Call(bt.root, f); }
    if (!AddWidgetToViewport(bt.root, zOrder)) {
        UE_LOGE("engine: SpawnScreenTextWidget -- AddToViewport failed");
        return false;
    }
    // The alignment is the pivot inside the widget placed at `position`: (0,0) top-left, (1,0)
    // top-right, (0.5,0.5) centre. Position pixels assume a 1080p viewport.
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

    // The reference button lives in a list panel, a vertical box; our button goes into that box,
    // so it auto-positions like the other items with no canvas-anchor arithmetic, then is
    // reordered to the top.
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

    // Label styling matches the native menu items deterministically rather than by cloning a
    // reference block, whose pointer is null at some inject timings (which fell through to a
    // default font, centred and white). The native labels are the menu font at size 16,
    // left-justified, no outline, a (2,2) opaque black shadow, per the menu's reflection dump;
    // that is set here, tinted cyan to mark the coop entry. The typeface name stays None, the
    // font's single default face.
    {
        auto* d = reinterpret_cast<uint8_t*>(txt);
        auto* font = d + P::off::UTextBlock_Font;
        // The native menu font, with the engine's default as a fallback so the label is always
        // readable.
        void* chosenFont = g_votvMenuFont ? g_votvMenuFont : g_npFont;
        if (chosenFont) *reinterpret_cast<void**>(font) = chosenFont;
        *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_Size) = 16;
        // No outline; the native labels have none.
        *reinterpret_cast<int32_t*>(font + P::off::FSlateFontInfo_OutlineSettings +
                                    P::off::FFontOutlineSettings_OutlineSize) = 0;
        // Cyan, with the use rule set to specified.
        *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ColorAndOpacity) =
            FLinearColor{0.f, 1.f, 1.f, 1.f};
        *(d + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
        // Left-justified, matching the native items.
        *(d + P::off::UTextLayoutWidget_Justification) = 0;
        // The drop shadow: offset (2,2), opaque black, exactly the native labels.
        *reinterpret_cast<FVector2D*>(d + P::off::UTextBlock_ShadowOffset) = FVector2D{2.f, 2.f};
        *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ShadowColorAndOpacity) =
            FLinearColor{0.f, 0.f, 0.f, 1.f};
        SetTextOnBlock(txt, label);
    }

    // The label goes inside the button through SetContent.
    { ParamFrame f(g_biSetContentFn); f.Set<void*>(L"Content", txt); Call(button, f); }

    // The label is left-aligned within the button: SetContent created a slot whose default
    // horizontal alignment is centre, which indented the label against the flush-left native
    // items. Fill plus zero padding lets the text block span the button and its left
    // justification pin the text to the edge. The slot is reached through the label's slot
    // back-pointer.
    if (void* cslot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(txt) + P::off::UWidget_Slot)) {
        auto* cs = reinterpret_cast<uint8_t*>(cslot);
        *(cs + P::off::UButtonSlot_HAlign) = 0;  // HAlign_Fill
        *(cs + P::off::UButtonSlot_VAlign) = 2;  // VAlign_Center
        std::memset(cs + P::off::UButtonSlot_Padding, 0, 0x10);  // FMargin = 0 (no left indent)
    }

    // Clone the reference button's visual style (the brushes, the tint, the press and hover
    // sounds) so ours matches. The clone lives in umg_build, since the native browser needs the
    // identical one and its correctness turns on which trailing bytes are zeroed.
    ue_wrap::umg::CloneButtonStyle(button, refButton);

    // Insert at the top of the box, above NEW GAME, through the shared reorder, which falls back to
    // a bottom append rather than leave the menu unrestorable.
    InsertAtTopOfVBox(listBox, button);

    // Match the reference item's slot layout (padding, alignment, size) so our button sits
    // exactly like NEW GAME. The slot was created fresh with defaults; the reference's layout
    // region is copied, excluding the base slot's parent and content pointers.
    {
        void* ourSlot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + P::off::UWidget_Slot);
        void* refSlot2 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refButton) + P::off::UWidget_Slot);
        if (ourSlot && refSlot2) {
            std::memcpy(reinterpret_cast<uint8_t*>(ourSlot) + P::off::UVerticalBoxSlot_LayoutStart,
                        reinterpret_cast<uint8_t*>(refSlot2) + P::off::UVerticalBoxSlot_LayoutStart,
                        P::off::UVerticalBoxSlot_LayoutSize);
        }
    }

    // Forced visible and input-receiving, so the hover and click poll sees it.
    if (g_widgetSetVisFn) {
        ParamFrame f(g_widgetSetVisFn); f.Set<uint8_t>(L"InVisibility", 0); Call(button, f);
    }

    if (outButton) *outButton = button;
    UE_LOGI("engine: InjectCanvasButton '%ls' button=%p listBox=%p", label, button, listBox);
    return true;
}

bool InjectTextRowAbove(void* refText, const wchar_t* initial,
                        void** outText, FLinearColor* outColor) {
    if (outText) *outText = nullptr;
    if (!refText) return false;
    // The button-inject resolve gives SpawnObject, the text-block class, the text setters and the
    // reorder's dependencies.
    if (!ResolveButtonInjectFns()) {
        UE_LOGE("engine: InjectTextRowAbove unresolved base (tbCls=%p spawn=%p)",
                g_npTbClass, g_npSpawnObjFn);
        return false;
    }

    // The menu's shape: the reference label sits in a row panel whose own slot lives in the rows
    // container, so the climb is two slot-to-parent hops, and inserting at the container's top
    // puts our block above every label row. The parent is a flow panel, not a canvas, so slot
    // offsets would have flowed the block to the right of the label.
    void* refSlot  = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refText) + P::off::UWidget_Slot);
    void* rowPanel = refSlot ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(refSlot) + P::off::UPanelSlot_Parent) : nullptr;
    void* rowSlot  = rowPanel ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(rowPanel) + P::off::UWidget_Slot) : nullptr;
    void* vbox     = rowSlot ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(rowSlot) + P::off::UPanelSlot_Parent) : nullptr;
    if (!vbox) {
        UE_LOGE("engine: InjectTextRowAbove -- row/vbox chain broken (refSlot=%p rowPanel=%p rowSlot=%p)",
                refSlot, rowPanel, rowSlot);
        return false;
    }

    void* txt = SpawnObject(g_npTbClass, vbox);
    if (!txt) { UE_LOGE("engine: InjectTextRowAbove SpawnObject(TextBlock) failed"); return false; }

    // Clone the reference label's text style so ours is visually identical. The font info is
    // copied field by field, not as the whole struct: its trailing shared pointer to the composite
    // font would be shallow-aliased with no reference added; the head is all scalars and
    // pointers, and Slate rebuilds the composite lazily from the font object, so leaving ours
    // empty is correct.
    {
        auto* d = reinterpret_cast<uint8_t*>(txt);
        auto* s = reinterpret_cast<uint8_t*>(refText);
        std::memcpy(d + P::off::UTextBlock_Font, s + P::off::UTextBlock_Font, 0x50);
        // The colour, a plain slate colour, safe to copy.
        std::memcpy(d + P::off::UTextBlock_ColorAndOpacity, s + P::off::UTextBlock_ColorAndOpacity, 0x18);
        if (outColor)
            *outColor = *reinterpret_cast<FLinearColor*>(s + P::off::UTextBlock_ColorAndOpacity);
        *reinterpret_cast<FVector2D*>(d + P::off::UTextBlock_ShadowOffset) =
            *reinterpret_cast<FVector2D*>(s + P::off::UTextBlock_ShadowOffset);
        *reinterpret_cast<FLinearColor*>(d + P::off::UTextBlock_ShadowColorAndOpacity) =
            *reinterpret_cast<FLinearColor*>(s + P::off::UTextBlock_ShadowColorAndOpacity);
        *(d + P::off::UTextLayoutWidget_Justification) = *(s + P::off::UTextLayoutWidget_Justification);
        SetTextOnBlock(txt, initial);
    }

    // Insert as the top row of the container, above every label row, then clone the reference
    // row's slot layout into our fresh slot, so our line carries the same indent and spacing.
    if (!InsertAtTopOfVBox(vbox, txt)) {
        UE_LOGE("engine: InjectTextRowAbove -- InsertAtTopOfVBox failed");
        return false;
    }
    {
        // Both slots re-read: the reorder destroyed and recreated every slot, so the pre-insert row
        // slot is dead. The helper restored the row's layout onto its new slot; that region is
        // cloned.
        void* ourSlot    = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(txt) + P::off::UWidget_Slot);
        void* rowSlotNew = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(rowPanel) + P::off::UWidget_Slot);
        if (ourSlot && rowSlotNew) {
            std::memcpy(reinterpret_cast<uint8_t*>(ourSlot) + P::off::UVerticalBoxSlot_LayoutStart,
                        reinterpret_cast<uint8_t*>(rowSlotNew) + P::off::UVerticalBoxSlot_LayoutStart,
                        P::off::UVerticalBoxSlot_LayoutSize);
        } else {
            UE_LOGW("engine: InjectTextRowAbove -- no slot to clone (our=%p row=%p)", ourSlot, rowSlotNew);
        }
    }

    if (outText) *outText = txt;
    UE_LOGI("engine: InjectTextRowAbove txt=%p vbox=%p (row inserted above %p)", txt, vbox, rowPanel);
    return true;
}

bool WidgetIsHovered(void* widget) {
    if (!widget || !ResolveButtonInjectFns() || !g_biIsHoveredFn) return false;
    ParamFrame f(g_biIsHoveredFn);
    if (!Call(widget, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool SetWidgetVisibility(void* widget, uint8_t slateVis) {
    // The button-inject resolve already resolves SetVisibility; reused.
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
