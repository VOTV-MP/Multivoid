// ue_wrap/engine/umg_build.h -- the UMG primitives more than one feature needs: panels and
// slots, Slate style cloning, widget setters, the scroll box, geometry. Engine-wrapper territory:
// nothing here knows about lobbies or peers; the browser's own composition lives in
// ui/server_browser_native.cpp. Not a widget framework: no builder, no retained model, no
// abstraction over a screen. Game thread only; every call reaches the engine through
// ProcessEvent.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::umg {

// Panels. UPanelWidget::AddChild is the one add that serves ScrollBox, Overlay, HorizontalBox,
// CanvasPanel and WidgetSwitcher alike; FindFunction matches the owning class with no
// super-walk, so it is resolved once on UPanelWidget. Returns the created slot, or nullptr.
void*   AddChild(void* panel, void* child);
bool    RemoveChild(void* panel, void* child);
int32_t ChildCount(void* panel);

// UPanelWidget::GetChildIndex: where this widget is, asked of the panel rather than inferred; -1
// when the child is not in the panel. A screen that adds itself to a shared container activates
// the index of its own widget, never the child count minus one, which is right only if the add
// succeeded and nothing was appended after it, and wrong it silently names one of the game's
// screens.
int32_t IndexOfChild(void* panel, void* child);
void*   ChildAt(void* panel, int32_t index);
int32_t IndexOfChild(void* panel, void* child);

// UContentWidget::SetContent, a SizeBox's, Button's or Border's single child. Latched, which is
// why it is here rather than open-coded: FindFunction walks the whole GUObjectArray, and seven
// unlatched copies, two of them per row, cost a full walk per row. False, logged once, if the
// class or function does not resolve.
bool    SetContent(void* contentWidget, void* child);

// The widget switcher.
bool    SwitcherSetIndex(void* switcher, int32_t index);
int32_t SwitcherIndex(void* switcher);

// Slate style cloning. FSlateBrush is 0x88 bytes and carries an unreflected shared-pointer
// resource handle at 0x70 (the reflected members end at 0x6F, the bitfield bools resume at
// 0x80), so a raw copy aliases a refcounted pointer with no reference added; Slate rebuilds the
// handle lazily from the resource object, so zeroing it is free and always correct. A table of
// brush offsets rather than one offset: FButtonStyle embeds four brushes, FScrollBarStyle nine,
// FEditableTextBoxStyle thirteen (a nested scrollbar style included), all measured from the
// header dump into sdk_profile.h. The zeroing is structural, not gated on any one donor's
// measurement.
bool CloneStyle(void* dst, size_t dstOff, void* src, size_t srcOff, size_t styleSize,
                const size_t* brushOffsets, int brushCount);

// Zero the unreflected handles of `brushCount` brushes in an already-copied style blob; exposed
// because the button inject clones a style that also carries two sound caches to zero.
void ZeroBrushHandles(void* styleBase, const size_t* brushOffsets, int brushCount);

// A whole UButton's look from one donor to another: the four-brush style, both sound resource
// objects and the two tints. The sounds are the point: each FSlateSound holds an unreflected
// shared-pointer cache past its resource object, and keeping the object while zeroing only the
// cache is what makes a cloned button play the native press and hover sounds. One owner; a
// second copy of a clone whose correctness turns on which trailing bytes to zero is what goes
// wrong on the next edit.
void CloneButtonStyle(void* dstButton, void* srcButton);

// Widget setters. The image tint through the SetBrushTintColor UFunction rather than a raw
// write: an image already attached to Slate would not repaint.
bool SetImageTint(void* image, const FLinearColor& tint);

// A UImage with no resource object and only a tint draws a solid rect (the game's own save-slot
// scrim is exactly that), so a scrim needs no donor and no art. A raw write; call before
// attaching.
bool SetImageTintRaw(void* image, const FLinearColor& tint);

// USizeBox height and width, through the UFunctions: the values are plain fields, but the
// override bits are a bitfield, and a raw write silently does nothing.
bool SetSizeBoxHeight(void* sizeBox, float height);
bool SetSizeBoxWidth(void* sizeBox, float width);

// Style a freshly spawned UTextBlock as one of the game's own menu labels: its UI font at the
// given size, the colour with the slate colour rule forced to specified, no outline, and the
// native drop shadow (the constants the button inject sets, which clones no donor text style,
// since that donor is null at some inject timings). `justify` is ETextJustify: Left 0, Center
// 1, Right 2. Raw writes: before the block is attached, or follow with a dispatch setter.
bool StyleTextBlock(void* textBlock, int32_t fontSize, const FLinearColor& color,
                    uint8_t justify);

// A raw colour write cannot recolour an already-attached block: UMG bakes properties into the
// Slate widget at attach. engine::SetTextBlockColorDispatch (the setter UFunction) is for
// anything in a constructed tree; StyleTextBlock owns build-time styling.

// UTextBlock::SetAutoWrapText: wrap a line instead of cutting it off. Clipping is right for a
// table cell, wrong for prose, where a truncated sentence reads as a rendering fault. Through
// the UFunction, so it lands on a constructed tree too.
bool SetAutoWrapText(void* textBlock, bool wrap);

// UWidget::SetClipping (Inherit 0, ClipToBounds 1). A text block in a weighted HorizontalBox
// slot overflows its column by default, since the slot bounds the layout, not the painting.
bool SetClipping(void* widget, uint8_t clipping);

// The scroll box, through the UFunctions. Three calls: ScrollOffsetOfEnd is the maximum
// scrollable offset (content minus viewport), so it answers "is there anything to scroll"
// directly. Each returns a bool and writes through a reference, since 0 is a legitimate answer
// to both getters and a sentinel would make an unresolved UFunction indistinguishable from a
// measurement.
bool SetScrollOffset(void* scrollBox, float offset);
bool ScrollOffset(void* scrollBox, float& out);
bool ScrollOffsetOfEnd(void* scrollBox, float& out);

// Where the view is, 0 to 1. Read this, not ScrollOffset, whenever the question is "did it
// move": GetScrollOffset echoes the request (asked for a million it returns a million, on an
// empty box too), since it reports the desired offset with no clamp, so a set-then-get through
// it is a tautology. This reads the scrollbar's own distance from the top, post-layout state;
// with ScrollOffsetOfEnd it makes a verdict possible.
bool ViewOffsetFraction(void* scrollBox, float& out);

// Geometry: where a widget is on screen, in desktop pixels, and how big it is. The answer to
// "point the cursor at that button", replacing arithmetic: a self-check that reconstructed a
// button's position from the window's design constants was a second implementation of the
// layout the engine had performed, went stale one commit later, and its failure was
// indistinguishable from the one it was built to find. `outTopLeft` is in the same space as
// the OS cursor calls (Slate's absolute space for a game window is desktop pixels, so no DPI
// factor appears); `outSize` is the allotted size, not the desired one. False, logged, if any
// link is unresolved, both outs untouched; a widget never painted has no cached geometry and
// legitimately reports a zero rect.
bool WidgetScreenRect(void* widget, FVector2D& outTopLeft, FVector2D& outSize);


// The OS cursor converted into the space WidgetScreenRect reports in, by Slate's own inverse
// transform rather than arithmetic here. `screenPos` is client pixels. False when unavailable,
// `out` untouched.
bool CursorToWidgetAbsolute(const FVector2D& screenPos, FVector2D& out);

// Why a widget takes no hits: the chain to the root, each link's live visibility logged
// (GetVisibility reads the built SWidget). One HitTestInvisible anywhere removes the whole
// subtree from the hit grid while leaving it visible, which no screenshot or click can
// localise. Diagnostic; not per frame.
void LogVisibilityChain(const char* tag, void* widget);

// Slot alignment, written raw at the offsets in sdk_profile.h. Horizontal: Fill 0, Left 1,
// Center 2, Right 3; vertical: Fill 0, Top 1, Center 2, Bottom 3.
bool SetSlotAlign(void* slot, size_t hAlignOff, size_t vAlignOff, uint8_t h, uint8_t v);

// The same alignment after the tree is live, through the slot's own SetHorizontalAlignment: the
// raw write is a no-op once the panel has constructed (UMG copies slot properties into the
// Slate slot then). Every slot type declares its own setter, and FindFunction does no
// super-walk, so this resolves against the slot's runtime class and caches per class. The
// consumer is the text field's overflow fix, which flips a slot to Right while the player
// types, so Slate clips the head and keeps the tail and the caret visible.
bool SetSlotHAlignLive(void* slot, uint8_t h);

// UWidget::GetDesiredSize, what the widget asked for, as opposed to what the parent gave it;
// desired over allotted is the overflow. Zero is a legitimate answer (never laid out), hence
// the bool and the reference.
bool WidgetDesiredSize(void* widget, FVector2D& out);

}  // namespace ue_wrap::umg
