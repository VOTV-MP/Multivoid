// ue_wrap/engine/umg_build.h -- the UMG primitives more than one feature needs.
//
// PRINCIPLE 7: this is engine-wrapper territory. Nothing here knows about lobbies, peers,
// or coop state; it wraps UMG panels, slots and Slate style structs and nothing else. The
// gameplay/network side lives under coop/ and the browser's own composition lives in
// ui/server_browser_native.cpp.
//
// WHY A NEW TU AND NOT engine_widget.cpp. That file is 612 LOC and owns a different
// concept -- the world-space nameplate and the two shipped MENU INJECTS, each a specific
// feature. These are generic panel/style operations with no feature attached, and adding
// them there would push it past the 800-LOC soft cap while mixing two concepts. It also
// carries its OWN header rather than growing `engine.h` (1117 LOC and already flagged);
// "one header per engine_*.cpp TU" is the extraction that file is waiting for anyway.
//
// WHY THIS IS NOT THE "REUSABLE WIDGET LAYER" section 8 REJECTED. That rejection was
// `OPUS_48_DISCIPLINE.md:196-197` -- no new framework before N>=3 working cases -- and it
// still stands: there is no builder, no retained model, no abstraction over "a screen".
// Every function here already has at least two callers the day it lands (the shipped
// InjectCanvasButton, the native browser, and coop/dev/native_ui_probe), which is the bar
// that was actually being applied.
//
// Game thread only, all of it: every call reaches the engine through ProcessEvent.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::umg {

// ---- panels ---------------------------------------------------------------------
// UPanelWidget::AddChild is the ONE add that serves ScrollBox, Overlay, HorizontalBox,
// CanvasPanel and WidgetSwitcher alike -- `R::FindFunction` matches the OWNING class with
// no super-walk (reflection.cpp:493), so resolving it once on UPanelWidget is what makes
// every panel type work. Returns the created UPanelSlot, or nullptr.
void*   AddChild(void* panel, void* child);
bool    RemoveChild(void* panel, void* child);
int32_t ChildCount(void* panel);
void*   ChildAt(void* panel, int32_t index);
int32_t IndexOfChild(void* panel, void* child);

// ---- widget switcher ------------------------------------------------------------
bool    SwitcherSetIndex(void* switcher, int32_t index);
int32_t SwitcherIndex(void* switcher);

// ---- Slate style cloning --------------------------------------------------------
// THE ONE BRUSH CLONE IN THE TREE, and the reason it takes a TABLE.
//
// `FSlateBrush` is 0x88 and carries an UNREFLECTED `FSlateResourceHandle` -- a TSharedPtr
// -- at +0x70 (its reflected members end at ImageType @0x6F; the bitfield bools resume at
// 0x80). Any raw copy therefore shallow-aliases a refcounted pointer with no AddRef, the
// same hazard the FSlateSound cache had. Slate rebuilds the handle lazily from
// ResourceObject, so zeroing it is free and always correct.
//
// A single "handle offset" parameter would not have survived contact: FButtonStyle embeds
// FOUR brushes, FScrollBarStyle NINE, and FEditableTextBoxStyle THIRTEEN (four of its own
// plus a nested FScrollBarStyle at +0x328). The caller passes the brush-offset table from
// sdk_profile.h; every table there is measured from the CXXHeaderDump.
//
// MEASURED 2026-08-25: 0/4 handles were populated on `ui_saveSlots_C.button_back`, 3 of
// whose 4 brushes DO carry a ResourceObject -- so there is no live bug on this build. The
// zeroing is structural correctness applied uniformly, NOT a fix gated on a measurement
// that covers one donor.
bool CloneStyle(void* dst, size_t dstOff, void* src, size_t srcOff, size_t styleSize,
                const size_t* brushOffsets, int brushCount);

// Zero the unreflected resource handles of `brushCount` brushes inside an already-copied
// style blob. Exposed because InjectCanvasButton clones a style that ALSO carries two
// FSlateSound caches it must zero separately.
void ZeroBrushHandles(void* styleBase, const size_t* brushOffsets, int brushCount);

// ---- widget setters -------------------------------------------------------------
// UImage's brush TINT, via the SetBrushTintColor UFunction rather than a raw write: the
// image may already be attached to Slate, and a raw property write would not repaint (the
// 2026-07-16 "no cyan" trap that SetTextBlockColorDispatch exists for).
bool SetImageTint(void* image, const FLinearColor& tint);

// A UImage with NO ResourceObject and only a tint DRAWS A SOLID RECT. That is not a
// guess: the game's own `ui_saveSlots_C.Image_302` is exactly that -- full-screen,
// TintColor (0,0,0,0.5), no texture -- and it visibly dims the menu behind every native
// sub-screen. So a scrim needs no donor and no art. Raw write; call BEFORE attaching.
bool SetImageTintRaw(void* image, const FLinearColor& tint);

// USizeBox height/width. MUST go through the UFunctions: the values live at +0x134/+0x130
// but the `bOverride_*` bits are a BITFIELD at +0x150, so a raw write silently does
// nothing at all.
bool SetSizeBoxHeight(void* sizeBox, float height);
bool SetSizeBoxWidth(void* sizeBox, float width);

// Style a freshly-spawned UTextBlock as one of VOTV's own menu labels: font_ui at the
// given size, the colour with FSlateColor's rule forced to UseColor_Specified, no outline,
// and the native (2,2) opaque-black drop shadow (measured from ui_menu's tex_btnStart --
// see InjectCanvasButton, which sets exactly these constants and does NOT clone a donor
// text style, because that donor is null at some inject timings and the fallback is the
// Roboto/centred/white bug). `justify` is ETextJustify: Left=0 Center=1 Right=2.
// Raw writes -- call BEFORE the block is attached, or follow with a dispatch setter.
bool StyleTextBlock(void* textBlock, int32_t fontSize, const FLinearColor& color,
                    uint8_t justify);

// UWidget::SetClipping. EWidgetClipping: Inherit=0, ClipToBounds=1. A text block in a
// weighted HorizontalBox slot OVERFLOWS its column by default -- the slot bounds the
// LAYOUT, not the painting -- so a long world name paints straight over the next column.
bool SetClipping(void* widget, uint8_t clipping);

// ---- scroll box -----------------------------------------------------------------
// UScrollBox's offset, through the UFunctions (UMG.hpp:1198,1211,1212).
//
// THREE CALLS, NOT TWO, AND THE THIRD IS THE ONE THAT DISCRIMINATES. `ScrollOffsetOfEnd`
// is the maximum scrollable offset, i.e. content extent minus viewport extent -- so it
// answers "is there anything here to scroll at all" DIRECTLY, rather than by inferring it
// from a row count against an assumed viewport height. A T0 that reads only the current
// offset cannot tell "the wheel did nothing" from "there was nowhere to go".
//
// EACH RETURNS bool AND WRITES THROUGH A REFERENCE, deliberately: 0.f is a LEGITIMATE
// answer to both getters (a list at the top; a list that does not overflow), so folding
// a failed call into a sentinel float would make an unresolved UFunction indistinguishable
// from a measurement. This module's whole job at T0 is to be an instrument that can fail
// visibly -- see docs/LESSONS.md section 7.
bool SetScrollOffset(void* scrollBox, float offset);
bool ScrollOffset(void* scrollBox, float& out);
bool ScrollOffsetOfEnd(void* scrollBox, float& out);

// WHERE THE VIEW ACTUALLY IS, 0..1 (UMG.hpp:1211). READ THIS, NOT ScrollOffset, WHENEVER
// THE QUESTION IS "DID IT MOVE".
//
// MEASURED 2026-08-26, twice: `GetScrollOffset` ECHOES THE REQUEST. Asked for 1000000 it
// returns 1000000 -- on an empty box AND on one holding 30 rows with 1391 units of real
// overflow. It reports Slate's DesiredScrollOffset, i.e. what was last ASKED FOR, and no
// clamp is applied to it. So a Set/Get round-trip through it is a tautology and can never
// fail, which is the one property an instrument must not have.
//
// GetViewOffsetFraction reads the scrollbar's own distance-from-top, which is physical
// post-layout state. The pair is what makes a verdict possible: ScrollOffset says what was
// requested, this says what happened, and OffsetOfEnd (also real geometry -- 1391.0 against
// 30x64 rows in a ~529 px viewport) says whether there was anywhere to go.
bool ViewOffsetFraction(void* scrollBox, float& out);

// Slot alignment, written raw at the offsets in sdk_profile.h. EHorizontalAlignment:
// Fill=0 Left=1 Center=2 Right=3; EVerticalAlignment: Fill=0 Top=1 Center=2 Bottom=3.
bool SetSlotAlign(void* slot, size_t hAlignOff, size_t vAlignOff, uint8_t h, uint8_t v);

}  // namespace ue_wrap::umg
