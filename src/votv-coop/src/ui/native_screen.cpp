// ui/native_screen.cpp -- see ui/native_screen.h for WHY.
//
// Every body here MOVED VERBATIM out of server_browser_native.cpp (2026-08-29). The
// comments came with them: they record measurements, and a measurement's comment is worth
// more where the code is than where the code used to be.

#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"   // the hit-space probe's one line
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>   // GetCursorPos -- HoverTracker reads the real pointer

#include <cmath>
#include <cstdlib>   // the hit probe's raisable cap reads one env var

namespace ui::native_screen {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace U = ue_wrap::umg;
namespace P = ue_wrap::profile;

}  // namespace

// THE sRGB CONVERSION IS NOT OPTIONAL. Those are sRGB byte values; FLinearColor is LINEAR,
// and the framebuffer converts back on the way out. Writing 0x31/255 = 0.192 as a linear
// tint puts sRGB 0.48 on screen -- #7B, more than double the intended #31 -- so the whole
// palette would render washed out and no amount of re-picking values would fix it. This is
// the same transform UE's FLinearColor::FromSRGBColor applies.
FLinearColor Srgb(int r, int g, int b, float a) {
    auto f = [](int v) {
        const float c = static_cast<float>(v) / 255.f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return FLinearColor{f(r), f(g), f(b), a};
}

FLinearColor Panel()  { return Srgb(0x1A, 0x1A, 0x1A); }
FLinearColor Border() { return Srgb(0x64, 0x64, 0x64); }
FLinearColor RowBg()  { return Srgb(0x31, 0x31, 0x31); }
FLinearColor RowSel() { return Srgb(0x40, 0x00, 0x40); }
FLinearColor Text()   { return Srgb(0xFF, 0xFF, 0xFF); }
FLinearColor Accent() { return Srgb(0xFF, 0x7C, 0x00); }
FLinearColor Hover()  { return Srgb(0xFF, 0xFF, 0x00); }
FLinearColor Amber()  { return Srgb(0xFF, 0xBC, 0x00); }
FLinearColor Dim()    { return Srgb(0xA5, 0xA5, 0xA5); }
FLinearColor Own()    { return Srgb(0x9E, 0xEA, 0xB3); }

void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// A sub-screen donor MUST be read off the switcher's own child list. `R::FindObjectByClass`
// returns a DIFFERENT non-CDO instance -- a WidgetBlueprint carries a widget-tree template
// that is not named `Default__`, so the CDO skip at reflection.cpp:511 does not exclude it
// -- and every donor field read through it comes back null. That produced a false finding
// on the probe's first run and would have forced this design to add a precondition it
// explicitly rejected. Measured 2026-08-25.
void* SwitcherChild(void* switcher, const wchar_t* className) {
    const int32_t n = U::ChildCount(switcher);
    for (int32_t i = 0; i < n && i < 64; ++i) {
        void* c = U::ChildAt(switcher, i);
        if (c && R::ClassNameOf(c) == className) return c;
    }
    return nullptr;
}

void* DonorField(void* owner, const wchar_t* field) {
    if (!owner) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(owner), field);
    return ReadPtr(owner, off);
}

void* Spawn(const wchar_t* cls, void* outer) {
    void* k = R::FindClass(cls);
    return k ? E::SpawnUObject(k, outer) : nullptr;
}

// Build one styled UTextBlock and put it in `panel`, with an optional horizontal-box slot
// fill weight (0 = leave the slot alone, i.e. auto-size).
void* AddText(void* panel, const wchar_t* initial, int32_t size, const FLinearColor& col,
              uint8_t justify, float fillWeight) {
    void* t = Spawn(P::name::TextBlockClass, panel);
    if (!t) return nullptr;
    U::StyleTextBlock(t, size, col, justify);
    E::SetWidgetText(t, initial);
    void* slot = U::AddChild(panel, t);
    if (slot && fillWeight > 0.f) {
        auto* s = reinterpret_cast<uint8_t*>(slot) + P::off::UHorizontalBoxSlot_Size;
        *reinterpret_cast<float*>(s + P::off::FSlateChildSize_Value) = fillWeight;
        *(s + P::off::FSlateChildSize_SizeRule) = 1;  // ESlateSizeRule::Fill
        U::SetSlotAlign(slot, P::off::UHorizontalBoxSlot_HAlign,
                        P::off::UHorizontalBoxSlot_VAlign, kFill, kCenter);
        // The SLOT bounds the layout, not the painting: without this a long world name
        // paints straight across the Age column. EWidgetClipping::ClipToBounds = 1.
        U::SetClipping(t, 1);
        // ...and clipping alone leaves a clipped value touching the next column, which
        // reads as a rendering fault rather than as a long name. FMargin is
        // {Left, Top, Right, Bottom}; a right gutter is the whole fix.
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) +
                                             P::off::UHorizontalBoxSlot_Padding);
        pad[0] = 0.f; pad[1] = 0.f; pad[2] = 18.f; pad[3] = 0.f;
    }
    return t;
}

// THE FRAME. Every panel, row, header strip and value cell in VOTV's menus is a bordered
// box with sharp corners (style doc section 3), and nothing in the game's UI floats
// unboxed. Two stacked UImages give exactly that: an outer one carrying the border colour
// and an inner one inset by the border width carrying the fill. A UImage with no
// ResourceObject and only a tint draws a solid rect, which is how the game's own
// full-screen scrim works -- so this needs no donor and no art.
//
// Returns the OVERLAY the caller should put content in; content lands above the fill.
void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx) {
    void* box = Spawn(L"Overlay", parent);
    if (!box) return nullptr;
    void* edge = Spawn(L"Image", box);
    void* face = Spawn(L"Image", box);
    if (!edge || !face) return nullptr;
    U::SetImageTintRaw(edge, Border());
    U::SetImageTintRaw(face, fill);
    // ESlateVisibility: Visible=0 Collapsed=1 HIDDEN=2 HitTestInvisible=3
    // SelfHitTestInvisible=4. The first version of this wrote 2 meaning "chrome, not a hit
    // target" and got HIDDEN -- so the frame and the panel fill never drew AT ALL, and the
    // screenshot's apparent "window" was nothing but the rows' own backgrounds stacked up
    // with the title and footer floating outside them. 3 is the one that means what 2 was
    // meant to mean: it draws, and it never eats a click.
    E::SetWidgetVisibility(edge, 3);
    E::SetWidgetVisibility(face, 3);
    if (void* s = U::AddChild(box, edge))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* s = U::AddChild(box, face)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                             P::off::UOverlaySlot_Padding);
        pad[0] = pad[1] = pad[2] = pad[3] = borderPx;
    }
    return box;
}

// A chrome UButton with a text label, styled from a donor UButton.
//
// A REAL UButton, not a text block with a click poll, and the reason is the SOUND. Cloning
// the style carries the game's own press and hover FSlateSounds, so our X clicks like the
// menu's own controls -- the same thing that makes the shipped MULTIPLAYER inject feel
// native. It also gets Slate's press visual for free.
//
// The LABEL is authored, never cloned. Cloning a donor UTextBlock's style was tried on the
// menu inject and REVERTED: the donor reads null at some timings and the silent fallback is
// Roboto/centred/white. StyleTextBlock writes the measured constants instead.
//
// The label's PADDING is what gives the button its hit area -- "X" is one glyph, and an
// unpadded button is a hit target the width of that glyph.
void* BuildButton(void* parent, void* donorBtn, const wchar_t* label, int32_t fontSize) {
    void* b = Spawn(P::name::ButtonClass, parent);
    if (!b) return nullptr;
    U::CloneButtonStyle(b, donorBtn);
    // WHAT DOES THE DONOR'S OWN LABEL ACTUALLY CARRY? (probe; RULE 2 exempt.)
    //
    // User report 2026-08-30, comparing our chrome to VOTV's: "the outer buttons
    // ('Back') are just colored and normal font, but the buttons which sit inside
    // the widget are pixelated font ('Reset' and 'Save')". `VOTV_UI_STYLE.md:89`
    // records the opposite -- "Monospace throughout, the game's font_ui. No
    // proportional text in a menu" -- so either the doc is one font short or the
    // difference is size/outline rather than face, and I am not going to settle
    // that by looking at a screenshot (that doc's own section 7 is a list of three
    // times looking gave the wrong answer here).
    //
    // We clone the donor's FButtonStyle but AUTHOR the label, so this reads what
    // the game itself put on the same button and logs it once per donor. One
    // armed run turns "two faces or one" into a measurement.
    static int sReported = 0;
    if (donorBtn && sReported < 4) {
        // A UButton is a UContentWidget, i.e. a panel with exactly one child.
        if (void* dt = U::ChildAt(donorBtn, 0)) {
            auto* d = reinterpret_cast<uint8_t*>(dt) + P::off::UTextBlock_Font;
            void* face = *reinterpret_cast<void**>(d);
            const int32_t sz = *reinterpret_cast<int32_t*>(d + P::off::FSlateFontInfo_Size);
            const int32_t ol = *reinterpret_cast<int32_t*>(
                d + P::off::FSlateFontInfo_OutlineSettings + P::off::FFontOutlineSettings_OutlineSize);
            UE_LOGW("native_screen[fontprobe] donor '%ls' label class=%ls font='%ls' size=%d "
                    "outline=%d -- ours will be font_ui size=%d outline=0",
                    R::ToString(R::NameOf(donorBtn)).c_str(), R::ClassNameOf(dt).c_str(),
                    face ? R::ToString(R::NameOf(face)).c_str() : L"<null>", sz, ol, fontSize);
            ++sReported;
        } else {
            UE_LOGW("native_screen[fontprobe] donor '%ls' has no content widget -- cannot read "
                    "the face the game gives its own buttons",
                    R::ToString(R::NameOf(donorBtn)).c_str());
            ++sReported;
        }
    }
    if (void* t = Spawn(P::name::TextBlockClass, b)) {
        // ORANGE, NOT WHITE, and this is the whole of "our buttons look bold".
        //
        // `measured` 2026-08-30 by sampling the user's own native captures: every button
        // label in VOTV is the accent orange -- `Hide all`, `Language`, `Binds`, `Back`,
        // `Reset all`, `Apply`, `Fix mailbox` on the Settings screen, and every gamemode
        // tab -- while WHITE is reserved for the window TITLE and body text. The ink
        // samples `#FF8900` off a compressed PNG against the palette's recorded `#FF7C00`;
        // the palette wins, because inventing a shade from one crop is exactly the mistake
        // VOTV_UI_STYLE.md section 7 already records against my own eye.
        //
        // The user read the difference as WEIGHT ("кнопки какие-то жирные"), and it is not:
        // scaled for the capture sizes our glyphs carry LESS ink than the game's (114 vs
        // 144 lit pixels over a comparable label). Pure white on near-black is simply the
        // maximum contrast the panel can hold, so it reads heavy. Nothing about the face
        // or the size changed here -- only the colour that made them shout.
        U::StyleTextBlock(t, fontSize, Accent(), kJustCenter);
        E::SetWidgetText(t, label);
        U::SetContent(b, t);
        // SetContent created the UButtonSlot; centre the glyph and pad it out.
        if (void* cslot = ReadPtr(t, static_cast<int32_t>(P::off::UWidget_Slot))) {
            auto* cs = reinterpret_cast<uint8_t*>(cslot);
            *(cs + P::off::UButtonSlot_HAlign) = kCenter;
            *(cs + P::off::UButtonSlot_VAlign) = kCenter;
            auto* pad = reinterpret_cast<float*>(cs + P::off::UButtonSlot_Padding);
            pad[0] = 16.f; pad[1] = 6.f; pad[2] = 16.f; pad[3] = 6.f;
        }
    }
    if (void* s = U::AddChild(parent, b))
        U::SetSlotAlign(s, P::off::UHorizontalBoxSlot_HAlign,
                        P::off::UHorizontalBoxSlot_VAlign, kCenter, kCenter);
    return b;
}

namespace {

// TWO answers. There used to be a third -- `Below`, meaning "this child starts under the
// cursor, so stop walking" -- and the walk that consumed it is gone (see ChildAtCursor):
// it rested on child order matching arranged top-to-bottom order, which a rebuilt or
// scrolled-out list does not honour, and one violation returned "no row" for the whole
// list. The early-out below survives as what it always was underneath: a cheap Miss.
bool Probe(void* panel, int32_t i, long cx, long cy,
           const ue_wrap::FVector2D& panelTl, const ue_wrap::FVector2D& panelSz) {
    void* child = U::ChildAt(panel, i);
    ue_wrap::FVector2D tl{}, sz{};
    if (!child || !U::WidgetScreenRect(child, tl, sz) || sz.X < 1.f || sz.Y < 1.f)
        return false;
    if (static_cast<long>(std::floor(tl.Y)) > cy) return false;
    // CLIPPED TO THE PANEL, and it is not decoration: a child scrolled out of view is not
    // arranged, so its cached geometry is whatever it was when it last WAS -- rows were
    // observed reporting positions above the list's own top edge -- and a stale rect must
    // not be allowed to claim a cursor that is inside the viewport.
    const float top = tl.Y > panelTl.Y ? tl.Y : panelTl.Y;
    const float bot = (tl.Y + sz.Y) < (panelTl.Y + panelSz.Y) ? (tl.Y + sz.Y)
                                                              : (panelTl.Y + panelSz.Y);
    if (bot <= top) return false;   // entirely scrolled out
    // floor, not a truncating cast: `static_cast<long>` rounds toward zero, so a negative
    // coordinate would round the opposite way and eat the left pixel column of every row.
    // (The original note said "on a monitor left of the primary (negative desktop X)" --
    // written when this function was fed DESKTOP coordinates. It is fed CLIENT pixels
    // now, which are negative only above/left of the client area, but the floor is still
    // the right call and costs nothing.)
    const bool in = cy >= static_cast<long>(std::floor(top)) &&
                    cy <  static_cast<long>(std::floor(bot)) &&
                    cx >= static_cast<long>(std::floor(tl.X)) &&
                    cx <  static_cast<long>(std::floor(tl.X + sz.X));
    return in;
}

}  // namespace

int32_t ChildAtCursor(void* panel, int32_t count, long cx, long cy, int32_t hint) {
    if (!panel || count <= 0) return -1;
    ue_wrap::FVector2D tl{}, sz{};
    if (!U::WidgetScreenRect(panel, tl, sz) || sz.X < 1.f || sz.Y < 1.f) return -1;
    if (cx < static_cast<long>(std::floor(tl.X)) ||
        cx >= static_cast<long>(std::floor(tl.X + sz.X)) ||
        cy < static_cast<long>(std::floor(tl.Y)) ||
        cy >= static_cast<long>(std::floor(tl.Y + sz.Y)))
        return -1;
    if (hint >= 0 && hint < count && Probe(panel, hint, cx, cy, tl, sz))
        return hint;
    for (int32_t i = 0; i < count; ++i) {
        if (i == hint) continue;   // already probed
        // NO EARLY BREAK ON `Below` (2026-08-30). This loop used to stop at the first
        // child whose top edge sits under the cursor, on the reasoning that children of
        // a vertical list are ordered top-to-bottom, so everything after it is further
        // down. That ordering is an ASSUMPTION about arranged geometry, and it is not
        // one this code is entitled to make:
        //
        //   * a child scrolled out of view is not arranged, so its rect is whatever it
        //     was when it last WAS -- the Probe above already documents rows "reporting
        //     positions above the list's own top edge", and a stale rect can just as
        //     easily read far BELOW;
        //   * the rows are rebuilt on every sync (12 -> 4 -> 12 in one lab run), and
        //     nothing in that path promises child order survives a rebuild.
        //
        // One out-of-order child therefore did not cost one row -- it ended the walk and
        // returned -1, i.e. NO row hovered anywhere, which is total rather than partial:
        // the selection path reads the same value, so no server could be picked either.
        // `measured` 2026-08-30: cursor (1600,772) sits inside child 6's own reported
        // rect (796,752) 955x64 -- all four bounds satisfied by hand -- while this
        // function returned -1, so the walk provably never reached it.
        //
        // The cost of correctness here is at most `count` rect reads on a list bounded
        // by kMaxRows, on a poll that only runs when the pointer or the scroll actually
        // moved. That is the right trade against losing the hit test outright.
        if (Probe(panel, i, cx, cy, tl, sz)) return i;
    }
    return -1;
}

bool CursorOverWidget(void* w) {
    if (!w) return false;
    POINT c{};
    if (!::GetCursorPos(&c)) return false;
    ue_wrap::FVector2D tl{}, sz{};
    if (!U::WidgetScreenRect(w, tl, sz) || sz.X < 1.f || sz.Y < 1.f) return false;
    return c.x >= static_cast<long>(std::floor(tl.X)) &&
           c.x <  static_cast<long>(std::floor(tl.X + sz.X)) &&
           c.y >= static_cast<long>(std::floor(tl.Y)) &&
           c.y <  static_cast<long>(std::floor(tl.Y + sz.Y));
}

void HoverTracker::Reset() {
    lastX_ = lastY_ = -1;
    lastFrac_  = -2.f;
    lastCount_ = -1;
    index_     = -1;
    pending_   = false;
}

bool HoverTracker::Poll(void* panel, int32_t shownCount) {
    POINT c{};
    if (!::GetCursorPos(&c)) return false;
    const bool moved = (c.x != lastX_ || c.y != lastY_);
    lastX_ = c.x; lastY_ = c.y;

    // THE POINTER IS NOT THE ONLY THING THAT MOVES A ROW UNDER IT. A wheel scroll moves the
    // rows while the cursor is still, and a sync can change how many there are. On failure
    // the fraction is left as it was rather than written to a sentinel, so an unreadable
    // scroll degrades to cursor-only rather than to a permanent re-evaluation.
    float frac = lastFrac_;
    U::ViewOffsetFraction(panel, frac);
    const bool scrolled = (frac != lastFrac_) || (shownCount != lastCount_);
    lastFrac_  = frac;
    lastCount_ = shownCount;

    if (!moved && !scrolled && !pending_) return false;
    // One settling pass is owed after motion stops: during a sweep the answer trails by a
    // frame, and without this it would stay trailing -- the next tick sees no delta, skips,
    // and nothing ever corrects it.
    pending_ = moved || scrolled;

    // THE CURSOR MUST BE IN THE RECT'S SPACE, AND IT WAS NOT.
    //
    // `GetCursorPos` is DESKTOP space; `WidgetScreenRect` composes Slate's own
    // `LocalToAbsolute`, whose output is CLIENT pixels. The two were compared
    // directly from the day this hit test was written, which is correct only when
    // the window happens to sit at the desktop origin -- i.e. it worked in
    // fullscreen and silently missed by the whole client origin in a window.
    //
    // MEASURED 2026-08-30 (VOTVCOOP_HIT_SPACE_PROBE, 1008 lines, all agreeing):
    //   cursor desktop=(1282,718) client=(962,538) clientOrigin=(320,180)
    //   panel (796,496) 968x470  ->  hit(desktop) = -1   hit(client) = 0
    // The pointer is physically on a row; the desktop comparison finds nothing
    // because it looks 180 px further down the list than the pointer actually is.
    //
    // Reported by the user hands-on ("их хитбокс находится не там где визуал"),
    // and NOT guessed at: the probe that produced the numbers above shipped first,
    // in its own commit, precisely because my hypothesis predicted the offset in
    // the opposite direction to the report. It fixes what it measures. If a small
    // residual offset survives in FULLSCREEN -- where this bug cannot manifest,
    // the origin being (0,0) -- that is a second cause and still open.
    POINT cli = c;
    if (HWND hwnd = ::GetActiveWindow()) ::ScreenToClient(hwnd, &cli);

    // ...AND THEN ASK SLATE, because the client origin was only half of it. After
    // the conversion above shipped, the same user reported the offset AGAIN and in
    // the OTHER direction (the top row selecting only with the cursor dragged well
    // down the widget). Two hand-derived corrections, two wrong answers: the
    // remaining term is the viewport's UI scale, and this file is not the place to
    // reconstruct Slate's transform from parts. `CursorToWidgetAbsolute` runs
    // Slate's own inverse, so both sides of the comparison come from one source
    // whatever the scale and wherever the window sits.
    ue_wrap::FVector2D abs{};
    const bool converted = U::CursorToWidgetAbsolute(
        ue_wrap::FVector2D{static_cast<float>(cli.x), static_cast<float>(cli.y)}, abs);
    const long hx = converted ? static_cast<long>(abs.X) : cli.x;
    const long hy = converted ? static_cast<long>(abs.Y) : cli.y;

    // ALWAYS-ON, first three hovers per process, at WARN so it FLUSHES. The user's
    // own run left no evidence at all last time -- INFO is buffered and a killed
    // process never writes it -- so a field report on this arrived with nothing to
    // read. Three lines is the price of never asking them to re-run with a flag.
    //
    // THE CAP IS RAISABLE FOR THE LAB (2026-08-30). Three lines are the right budget
    // for a player, and exactly the wrong one for the selftest: the browser run spends
    // all three before the ROW phase begins, so the one moment the lane needs to see --
    // the cursor placed on a row, the hit test's own answer for it -- was the moment
    // the probe had already fallen silent. `VOTVCOOP_HIT_PROBE=N` lifts it; the lab
    // sets it, and nothing a player runs does.
    static const int sCap = [] {
        if (const char* v = std::getenv("VOTVCOOP_HIT_PROBE")) {
            const int n = std::atoi(v);
            if (n > 0) return n;
        }
        return 3;
    }();
    static int sTold = 0;
    if (moved && sTold < sCap) {
        ++sTold;
        ue_wrap::FVector2D ptl{}, psz{};
        const bool haveP = U::WidgetScreenRect(panel, ptl, psz);
        const int32_t hit = ChildAtCursor(panel, shownCount, hx, hy, -1);
        UE_LOGW("native_screen[hit] desktop=(%ld,%ld) client=(%ld,%ld) slateAbs=%s(%.1f,%.1f) "
                "panel %s(%.0f,%.0f) %.0fx%.0f -> row=%d",
                c.x, c.y, cli.x, cli.y, converted ? "" : "UNCONVERTED ", abs.X, abs.Y,
                haveP ? "" : "UNREAD ", ptl.X, ptl.Y, psz.X, psz.Y, hit);
        // A MISS INSIDE THE PANEL IS THE ONLY INTERESTING MISS, and it is the one that
        // has now survived two hand-derived fixes. Both were reasoned from the ONE rect
        // the selftest happened to dump; neither author had ever seen the other eleven.
        // So when the cursor is inside the list and no child claims it, print the whole
        // child table -- index, rect, and whether the rect was readable at all -- because
        // the answer is a COMPARISON across children, and no single-row dump can carry it.
        if (hit < 0 && haveP && sCap > 3 &&
            hx >= static_cast<long>(ptl.X) && hx < static_cast<long>(ptl.X + psz.X) &&
            hy >= static_cast<long>(ptl.Y) && hy < static_cast<long>(ptl.Y + psz.Y)) {
            UE_LOGW("native_screen[hit]   MISS INSIDE THE PANEL -- %d child(ren) follow",
                    shownCount);
            for (int32_t i = 0; i < shownCount && i < 64; ++i) {
                void* ch = U::ChildAt(panel, i);
                ue_wrap::FVector2D ctl{}, csz{};
                const bool haveC = ch && U::WidgetScreenRect(ch, ctl, csz);
                UE_LOGW("native_screen[hit]     child %2d %ls %s(%.0f,%.0f) %.0fx%.0f%s",
                        i, ch ? R::ClassNameOf(ch).c_str() : L"(null)",
                        haveC ? "" : "UNREAD ", ctl.X, ctl.Y, csz.X, csz.Y,
                        (haveC && hx >= static_cast<long>(ctl.X) &&
                         hx < static_cast<long>(ctl.X + csz.X) &&
                         hy >= static_cast<long>(ctl.Y) &&
                         hy < static_cast<long>(ctl.Y + csz.Y)) ? "  <== CONTAINS CURSOR" : "");
            }
        }
    }

    index_ = ChildAtCursor(panel, shownCount, hx, hy, index_);
    return true;
}

}  // namespace ui::native_screen
