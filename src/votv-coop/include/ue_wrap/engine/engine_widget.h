// ue_wrap/engine/engine_widget.h -- runtime UMG: spawning widgets and text blocks, writing their
// text and colour, and putting them in the viewport. Engine-wrapper layer (principle 7): each call
// marshals one UFunction call or one reflected field access, with no gameplay, network or coop
// state. Game thread unless a declaration says otherwise. Implementation:
// src/ue_wrap/engine/engine_widget.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::engine {

// Build a screen-space HUD widget (a UUserWidget with one multi-line UTextBlock) and add it to the
// viewport, HitTestInvisible; `outer` should be the GameInstance so it survives level loads.
// alignment = the pivot ({0,0} top-left, {1,0} top-right), position = viewport pixels, justify =
// ETextJustify (0 left, 1 centre, 2 right), fontSize in points; outRoot for a re-attach, outText
// for SetWidgetText. Game thread.
bool SpawnScreenTextWidget(void* outer, int zOrder, FVector2D alignment, FVector2D position,
                           int justify, int fontSize, const FLinearColor& color,
                           void** outRoot, void** outText);

// NewObject by class: the reflected UGameplayStatics::SpawnObject(objectClass, Outer), the one way
// this mod mints a UObject. `outer` must keep the result reachable: a widget's Outer does not root
// it, a panel's `Slots` UPROPERTY does, so an unattached widget is collectable at the next GC.
// nullptr until SpawnObject resolves. Game thread.
void* SpawnUObject(void* objectClass, void* outer);

// Set a UTextBlock's text (Conv_StringToText, then SetText). Game thread.
bool SetWidgetText(void* textBlock, const wchar_t* text);

// Raw write of a UTextBlock's ColorAndOpacity (ColorUseRule forced to UseColor_Specified). Only a
// UWidgetComponent-hosted block re-renders from its properties; a block in a constructed Slate tree
// ignores it (SetTextBlockColorDispatch). Game thread.
bool SetTextBlockColor(void* textBlock, const FLinearColor& color);

// UTextBlock::SetColorAndOpacity, the setter: required in a constructed UMG tree, where UMG bakes
// properties into Slate at attach. Game thread.
bool SetTextBlockColorDispatch(void* textBlock, const FLinearColor& color);

// UUserWidget::AddToViewport / RemoveFromViewport: re-attach the HUD feed after a level load (the
// object survives; its Slate tree does not). Game thread.
bool AddWidgetToViewport(void* userWidget, int zOrder);

bool RemoveWidgetFromViewport(void* userWidget);

// ---- Runtime UMG button injection ----
// Insert a UButton at the top of `refButton`'s UVerticalBox, cloning its FButtonStyle and slot
// layout so spacing and indent match, with the label styled as the native items (font_ui at 16,
// left-justified, cyan). `outButton` (may be null) gets the UButton* for the click poll. Game
// thread.
bool InjectCanvasButton(void* refButton, const wchar_t* label, void** outButton);

// Inject a UTextBlock as a new row above `refText`'s (a UHorizontalBox row inside a UVerticalBox of
// label rows), cloning the row's slot layout and refText's text style, so the coop version line
// reads as one more native label and shows with the menu. `outColor` gets the cloned colour so a
// temporary tint can be restored. Game thread.
bool InjectTextRowAbove(void* refText, const wchar_t* initial,
                        void** outText, FLinearColor* outColor);

// UWidget::IsHovered, the mouse-over test for the menu click poll. Game thread.
bool WidgetIsHovered(void* widget);

// UWidget::SetVisibility (0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4
// SelfHitTestInvisible). The loading state hides the game's menu with HitTestInvisible plus
// SetWidgetRenderOpacity(0): still a rendered state, so the menu keeps ticking and the same
// observer can restore it. Game thread.
bool SetWidgetVisibility(void* widget, uint8_t slateVis);

// UWidget::SetRenderOpacity: the visual half of the menu hide. Game thread.
bool SetWidgetRenderOpacity(void* widget, float opacity);

}  // namespace ue_wrap::engine
