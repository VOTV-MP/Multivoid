// ui/server_browser_rows.cpp -- see ui/server_browser_rows.h.

#include "ui/server_browser_rows.h"

#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"       // the ONE owner of text encoding (CLAUDE.md 4a-names)
#include "ui/native_screen.h"           // palette + widget primitives, shared with the host window
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <string>
#include <vector>

namespace ui::server_browser_rows {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace sm = coop::session_manager;
namespace NS = ui::native_screen;

using ue_wrap::FLinearColor;
using Row = coop::net::lobby::LobbyRow;

using NS::kFill;
using NS::kCenter;
using NS::kJustLeft;
using NS::AddText;
using NS::Spawn;

// Slate units. The row height is the game's own (uicomp_saveSlot_C is 64 px, section 7b),
// so a row of ours is the same size as a row of theirs.
constexpr float kRowH     = 64.f;
constexpr float kRowGapPx = 2.f;
// EVERY ROW CARRIES ITS OWN FRAME. `docs/VOTV_UI_STYLE.md` section 3 rule 1: nothing in
// VOTV's UI floats unboxed, and section 5 listed per-row borders as the last open style
// gap -- deferred at the time on the widget budget, because a frame costs one more UImage
// per row and that cost is the subject of the open perf lane. The user asked for it
// directly on 2026-08-30 ("чтобы из списка серверы не сливались, у каждого свои границы"),
// which settles the trade: a list where adjacent rows blend into one slab is not a list.
//
// The frame is INSIDE the 64 px row, so it changes no layout arithmetic -- the scroll
// clamp's `want * (kRowH + kRowGapPx)` still describes the content height. The 2 px slot
// gap STAYS: two adjacent 2 px frames with no gap read as one 4 px rule between rows,
// which is the blending this is meant to end.
constexpr float kRowBorderPx = 2.f;
// BOUNDS THE WHOLE SYNC LOOP, not just the display: `want` clamps to this, the grow
// loop is bounded by `want`, and `total = ChildCount` therefore never exceeds it. So
// this is a COMPUTE ceiling that happens to look like a display one.
// The comment that stood here -- "the master caps its list well below this" -- was
// FALSE (2026-08-26): `build_rows` (master.rs:531-553) emits every listed lobby,
// unbounded, so 100 servers render 64. It no longer does so SILENTLY (see the log in
// `Sync`), but the rows are still not drawn.
//
// RAISING IT IS STEP T2b, AND ITS STATED BLOCKER IS GONE -- which does not make it free.
// The old reason was the uncached `FindFunction` walk per row; that is latched now, and
// the surplus-row branch no longer derives seven parts to collapse one. What remains is
// ~28 ProcessEvent dispatches per PAINTED row (10 to derive the parts, 10 to write five
// texts, 2 for the frame's two images, 5 to dispatch their colours, 1 visibility), all
// landing in one frame, so 200 rows is ~5,600 dispatches in a single sync. The figure was
// 21 until 2026-08-30, when the per-row frame added one image and the text colours became
// setter DISPATCHES instead of raw writes that never reached the screen; a post-ship audit
// caught this line still quoting the old number, which is the number T2b would be priced
// on. That is arithmetic, not a measurement: T2a's
// instrument and T2c's baseline have not run, and this project's own rule is that a walk
// cost without its denominator on the same line is not a number. Raise it after the
// baseline, not before.
constexpr int   kMaxRows  = 64;

const FLinearColor kRowBg  = NS::RowBg();   // a list row at rest
const FLinearColor kRowSel = NS::RowSel();  // ...and selected. Fill, not text.
const FLinearColor kBorder = NS::Border();  // #646464 -- every frame in the game's menus
const FLinearColor kText   = NS::Text();    // the default: most text is white
const FLinearColor kAccent = NS::Accent();  // orange -- the interactive accent
const FLinearColor kHover  = NS::Hover();   // #FFFF00 -- hover, on the text AND the frame
const FLinearColor kAmber  = NS::Amber();   // value emphasis; the mismatch tint
const FLinearColor kDim    = NS::Dim();     // secondary text (measured, not guessed)
const FLinearColor kOwn    = NS::Own();     // "your server" row

// ---- state (GAME THREAD ONLY) -----------------------------------------------------
void* g_list = nullptr;   // the UScrollBox holding the rows

// HOVER AND SELECTION ARE DIFFERENT CHANNELS, AND SELECTION OUTRANKS HOVER.
//
// Style doc section 4, measured: native hover is a TEXT colour change to #FFFF00 and
// native selection is a row FILL change to #400040 -- the fill is identical on hovered and
// unhovered native rows, which is why porting ImGui's paint-behind-the-row look would read
// as foreign. Both channels are applied on a CHANGE, never per frame.
//
// TWO THINGS ARE ADDED HERE ON TOP OF THAT MEASUREMENT, both on the user's 2026-08-30
// instruction, and both are deliberate rather than discovered:
//
//   * HOVER ALSO YELLOWS THE ROW'S FRAME. Native's text-only hover was measured on
//     SETTINGS rows -- one label and one value. Ours is a five-column data row roughly 640
//     px wide, where recolouring the glyphs alone is a change the eye does not find; the
//     user's word for what they wanted is "выделение", a highlight. The frame is the same
//     #FFFF00 the measurement produced, applied at row scale, so nothing here invents a
//     colour -- section 2's rule is that a value not in the palette is a mistake, and this
//     is not a new value.
//   * A SELECTED ROW IGNORES HOVER ENTIRELY. Verbatim: "если сервер из списка кликнут, то
//     выделение держится только на нем, а hover игнорится". So the pointer moving over the
//     row a player already chose changes nothing about it -- neither its frame nor its
//     text. The purple says "this is the one", and nothing transient is allowed to argue.
//
// The consequence worth stating: hover and selection can never fight over one pixel. Hover
// owns the FRAME and the TEXT, selection owns the FILL, and where both would apply,
// selection wins by suppressing hover at the source rather than by painting over it.
int              g_hoverRow = -1;   // index into the live rows, or -1
NS::HoverTracker g_hover;           // pointer + scroll + settling, one owner
std::string      g_selectedId;      // the SELECTED LOBBY, keyed by id and not by index

// Row identity. See the header's invariant: `g_rowIds[i]` is the lobbyId the child at
// index i was RENDERED with, written in the same pass as that child's text by the single
// function that owns the panel. Positional INSIDE this one structure is fine; positional
// across the network list and the displayed list is the defect this exists to avoid.
std::vector<std::string> g_rowIds;
std::vector<Row> g_rows;
uint64_t g_lastRowsGen  = 0;   // the fetch generation last PAINTED
int      g_visibleRows  = 0;   // rows actually SHOWN, not ChildCount's high-water mark

// The five columns, in the order section 7c fixes them, with their fill weights.
struct Column { const wchar_t* title; float weight; };
constexpr Column kColumns[5] = {
    {L"Name",    0.44f},
    {L"Players", 0.12f},
    {L"Version", 0.20f},
    {L"World",   0.14f},
    {L"Age",     0.10f},
};

// Build one row: USizeBox(64) -> UOverlay -> [ UImage EDGE, UImage FACE inset by the
// border, UHorizontalBox of five UTextBlocks ]. NO UButton: the native row's own
// `button_select` draws nothing in all three states, and a UButton would add a press
// visual we would then suppress.
//
// THE FRAME COMES FROM THE SHARED KIT, not from a second implementation here. The two
// images plus the inset are exactly `native_screen::AddFramedBox`, which already draws
// every panel, header strip and footer on this screen and on the hosting window; a copy
// of it in the row builder is the drift this project has paid for twice in one day (the
// kit's own header says so). It returns the OVERLAY, so the text row is simply the third
// child of what it built.
//
// THE FRAME IMAGES ARE HIT-TEST-INVISIBLE, and that is safe HERE for a reason worth
// writing down. The comment this replaces said the row background "is the hit target, so
// it must be Visible". That WAS true of the hover implementation it was written for, which
// asked Slate `IsHovered` on the background image. It is not true of the one that shipped
// on 2026-08-29: `native_screen::ChildAtCursor` -> `Probe` reads the rect of
// `ChildAt(panel, i)` -- the row's SIZEBOX -- and never touches the images at all
// (`native_screen.cpp:188-210`). Nothing else asks either image a hit-test question. So
// the images owe only their pixels, and hit-testing is the SizeBox's job, which `Sync`
// already sets Visible explicitly.
void* BuildRow(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    if (!box) return nullptr;
    U::SetSizeBoxHeight(box, kRowH);
    void* ovl = NS::AddFramedBox(box, kRowBg, kRowBorderPx);
    void* hb  = ovl ? Spawn(L"HorizontalBox", ovl) : nullptr;
    if (!ovl || !hb) return nullptr;
    if (void* s = U::AddChild(ovl, hb))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    for (const Column& c : kColumns)
        AddText(hb, L"", 16, kText, kJustLeft, c.weight);
    // SizeBox is a UContentWidget: its single child goes through SetContent.
    U::SetContent(box, ovl);
    return box;
}

// A row's parts, re-derived from the panel on demand. We hold NO row pointers across ticks
// (cached_obj_ref.h:34-46 -- the world stamp is inert for UMG, and UE assigns serials
// lazily, so a hand-spawned widget captures serial 0 and the ABA residual is not closed).
// The panel's `Slots` is the authority; this walks it.
struct RowParts { void* box; void* edge; void* face; void* text[5]; };
bool RowPartsAt(int32_t i, RowParts& out) {
    out = RowParts{};
    void* box = U::ChildAt(g_list, i);
    if (!box) return false;
    out.box = box;
    void* ovl = nullptr;
    // LATCHED. `R::FindFunction` has no result cache and walks the whole GUObjectArray, and
    // this runs PER ROW inside Sync -- 64 rows meant 64 full walks
    // landing in one frame, every sync. `R::FindClass` beside it has been cached since
    // `ca1cd5e4`; only this one was not. A UFunction never moves, so one resolve is all
    // there is. (Post-ship perf audit, 2026-08-29. The general fix -- a cache inside
    // `FindFunction` for all 476 call sites -- is filed separately and is not this lane.)
    // ONCE, INCLUDING ON FAILURE. The first version latched only success, so a miss re-walked
    // the whole GUObjectArray on every call -- and this runs per ROW, so the failure path was
    // 64 full walks per frame. A UFunction, unlike an asset, either exists at process start
    // or never does, so a permanent negative latch is not merely safe here, it is correct.
    static void* const sGetContent = [] {
        void* cw = R::FindClass(P::name::ContentWidgetClass);
        return cw ? R::FindFunction(cw, P::name::GetContentFn) : nullptr;
    }();
    if (sGetContent) {
        ue_wrap::ParamFrame f(sGetContent);
        if (Call(box, f)) ovl = f.Get<void*>(L"ReturnValue");
    }
    if (!ovl) return false;
    // The overlay's children, in BuildRow's order: the frame's edge, the frame's face, the
    // text row. The first two are AddFramedBox's, so if that kit function ever reorders
    // them this reads the wrong image -- which is why the row builder uses the kit rather
    // than hand-rolling a frame whose order only this file knows.
    out.edge = U::ChildAt(ovl, 0);
    out.face = U::ChildAt(ovl, 1);
    void* hb = U::ChildAt(ovl, 2);
    if (!hb) return false;
    for (int c = 0; c < 5; ++c) out.text[c] = U::ChildAt(hb, c);
    return true;
}

// The version cell, PORTED from the incumbent rather than re-derived (server_browser.cpp:
// 218-231). `game` is the host's VOTV target and `version` is a LEGACY-ONLY fallback that
// only a pre-field host still sends; the mismatch has TWO legs and amber always means
// "the join gate will refuse this".
std::wstring VersionCell(const Row& r, bool& mismatch) {
    const bool gameMismatch = !r.game.empty() && r.game != sm::GameTarget();
    mismatch = gameMismatch ||
               (r.proto > 0 && r.proto != static_cast<int>(coop::net::kProtocolVersion));
    std::string cell = !r.game.empty() ? r.game : r.version;
    if (r.proto > 0) cell += " b" + std::to_string(r.proto);
    if (mismatch) cell += " (!)";
    return coop::text::FromUtf8Lossy(cell.data(), cell.size());
}

// DOES THE POINTER GET TO CHANGE THIS ROW AT ALL. The single expression of the user's
// rule, so the three painters below cannot disagree about it: a SELECTED row is deaf to
// hover, in every channel. Written once and called, rather than repeated as `hovered &&
// !selected` at each site -- the same `&& !` copied three times is how one of them ends up
// missing the negation.
bool PointerLit(bool hovered, bool selected) { return hovered && !selected; }

// A row's TWO FILL CHANNELS, from its state. ONE owner, for the reason the text colours
// have one: the sync pass, the hover edge and the selection repaint all come through here,
// so "what colour is this row's frame" is answered in exactly one place.
//
// FILL says CHOSEN (#400040, persistent, set by a click). FRAME says POINTED AT (#FFFF00,
// transient, set by the cursor) and falls back to the ordinary #646464 every native box
// carries. They are independent writes to independent widgets, so neither can clobber the
// other; the precedence lives entirely in `PointerLit`.
void ApplyRowSkin(const RowParts& rp, bool hovered, bool selected) {
    // SetImageTint, not ...Raw: these images are already attached to Slate, and a raw
    // property write does not repaint (umg_build.h:99-108).
    if (rp.face) U::SetImageTint(rp.face, selected ? kRowSel : kRowBg);
    if (rp.edge) U::SetImageTint(rp.edge, PointerLit(hovered, selected) ? kHover : kBorder);
}

// A row's five text colours, from its data and its state. ONE owner: the sync pass
// and the hover edge both come through here, so "what colour is the version cell" is
// answered in exactly one place and un-hovering cannot restore the wrong base.
void ApplyRowTextColors(const RowParts& rp, const Row& r, bool isOwn, bool mismatch,
                        bool hovered, bool selected) {
    // Style doc section 4: hovering turns the LABEL yellow and leaves the FILL alone. That
    // is the opposite of the ImGui incumbent, which paints behind the row. A selected row
    // keeps its data colours on the purple -- section 4 measured that selection leaves the
    // content white -- and the pointer does not override that.
    const bool lit = PointerLit(hovered, selected);
    const FLinearColor name = lit ? kHover : (isOwn ? kOwn : kText);
    const FLinearColor body = lit ? kHover : kText;
    const FLinearColor dim  = lit ? kHover : kDim;
    const FLinearColor ver  = lit ? kHover : (mismatch ? kAmber : kDim);
    // ...Dispatch, NEVER the raw variant, AND EVERY COLOUR ON THIS SCREEN DEPENDED ON IT.
    //
    // `SetTextBlockColor` is a raw property write, and its own declaration says in capitals
    // that it "never propagates" to a block living in a CONSTRUCTED UMG/Slate tree, because
    // UMG bakes the property into the Slate widget at attach; it survives only for the
    // WidgetComponent nameplates, which re-render from properties every frame
    // (`engine.h:516-531`). These rows are a constructed tree.
    //
    // MEASURED 2026-08-30, by sampling glyph pixels out of the self-check's own capture --
    // not inferred. On the hovered row the frame read #FFFF00 (77 px) and its text read
    // #FFFFFF (216 px); the World and Age cells, which are supposed to be #A5A5A5, read
    // #FFFFFF too. So NOTHING this function has ever written reached the screen: not hover,
    // not the dim secondary columns, not the green "your server" name, and not the AMBER
    // VERSION MISMATCH -- which is the cue that the join gate will refuse that server, and
    // is therefore the one that was a correctness defect rather than a cosmetic one.
    //
    // Why it went unnoticed for four days: the COLUMN HEADERS are orange and always were
    // (#FF7C00, 165 px in the same capture). They are coloured at BUILD time, before the
    // widget is attached, where a raw write does land -- so the screen looked like a screen
    // whose colours work. `multiplayer_menu.cpp:155` already used the dispatch variant, for
    // exactly this reason, on exactly this kind of tree.
    if (rp.text[0]) E::SetTextBlockColorDispatch(rp.text[0], name);
    if (rp.text[1]) E::SetTextBlockColorDispatch(rp.text[1], body);
    if (rp.text[2]) E::SetTextBlockColorDispatch(rp.text[2], ver);
    if (rp.text[3]) E::SetTextBlockColorDispatch(rp.text[3], dim);
    if (rp.text[4]) E::SetTextBlockColorDispatch(rp.text[4], dim);
    (void)r;
}

// DECODED, NOT WIDENED. `std::wstring(s.begin(), s.end())` copies each BYTE into a wchar,
// so a Cyrillic or accented server name -- which arrives from the master as UTF-8 --
// renders as one garbage glyph per byte. This screen shipped with that widening and
// became the DEFAULT browser on 2026-08-30, which turned a lab defect into every
// non-ASCII player's server name. `coop/text/utf8_codec.h` is the project's single owner
// of text encoding (CLAUDE.md 4a-names); the lossy decode is the right one here because a
// browser row must still DRAW when a hostile master sends ill-formed bytes -- refusing
// the field whole is the receive boundary's job, not the renderer's.
//
// TEXT ONLY -- the colour is NOT set here. It used to be, and the write was dead twice
// over: dead because it was the raw variant (see ApplyRowTextColors), and redundant
// because `ApplyRowTextColors` runs a few lines later in the same pass and is documented
// as the ONE owner of a row's five colours. Two writers for one property is how a base
// colour comes back wrong after an un-hover; removing this one also pays for the dispatch
// the other one now has to make.
void SetRowText(void* block, const std::string& utf8) {
    if (!block) return;
    const std::wstring w = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
    E::SetWidgetText(block, w.c_str());
}

// THE ORDER INVARIANT, WATCHED RATHER THAN ASSUMED.
//
// The list is sorted at its one producer (lobby_client.cpp, at the parse site) so that the
// SAME SET of lobbies always renders in the SAME ORDER. That is a property of code the
// browser does not own and cannot see -- and the failure it prevents is silent, because a
// permuted list looks exactly like a list that changed.
//
// So the rows keep two digests of what they rendered: one over the id SEQUENCE, one over
// the id SET. Same set + different sequence is the defect, and it is the only combination
// that is. A changed set says nothing (a lobby came or went, and rows are allowed to move).
//
// It watches production, not a fixture: this fires against the live master too, so a future
// change that reintroduces a mutable sort key -- ordering by player count is the tempting
// one -- announces itself instead of being felt as rows twitching under the hand.
//
// KNOWN BLIND SPOT, and it is the case the truncation log proves exists: above `kMaxRows`
// the VISIBLE set is a window onto a longer list, so the window's membership changes
// whenever the list does and the same-set precondition simply never holds. The detector
// cannot see a reorder it would otherwise catch there. Raising the cap (T2b) removes the
// window; digesting the whole network list instead of the rendered one would not, because
// the claim this makes is about what the PLAYER saw.
struct OrderDigest { uint64_t sequence = 0; uint64_t set = 0; };

OrderDigest DigestIds(const std::vector<std::string>& ids, int count) {
    OrderDigest d;
    d.sequence = 1469598103934665603ull;   // FNV-1a offset basis
    for (int i = 0; i < count && i < static_cast<int>(ids.size()); ++i) {
        // THE INDEX IS MIXED IN, and only for the SET term. Two rows whose RowPartsAt
        // failed both carry an EMPTY id, and two identical hashes XOR to zero -- so a
        // pair of paint failures would silently erase itself from the set digest and
        // the detector would go quiet for a reason that has nothing to do with order.
        // Seeding with the index makes every slot distinct; the SEQUENCE term below is
        // already positional by construction. (Post-ship audit 2026-08-30, F6.)
        uint64_t one = 1469598103934665603ull ^ static_cast<uint64_t>(i);
        for (unsigned char c : ids[static_cast<size_t>(i)]) {
            one ^= c; one *= 1099511628211ull;
            d.sequence ^= c; d.sequence *= 1099511628211ull;
        }
        d.sequence ^= 0xFF; d.sequence *= 1099511628211ull;   // separator: order matters here
        d.set ^= one;                                          // XOR: order does NOT matter here
    }
    return d;
}

void CheckOrderStable(int shown) {
    static OrderDigest sPrev;
    static bool sHave = false;
    const OrderDigest now = DigestIds(g_rowIds, shown);
    if (sHave && now.set == sPrev.set && now.sequence != sPrev.sequence) {
        UE_LOGE("server_browser_rows: THE SAME %d LOBBIES RENDERED IN A DIFFERENT ORDER "
                "(set digest %016llx unchanged, sequence %016llx -> %016llx). Rows move "
                "under the player's hand and a scroll position means nothing. The order is "
                "imposed at the one producer -- lobby_client.cpp's sort at the parse site "
                "-- so this fires only if that sort was removed or given a key that MOVES",
                shown, static_cast<unsigned long long>(now.set),
                static_cast<unsigned long long>(sPrev.sequence),
                static_cast<unsigned long long>(now.sequence));
    }
    sPrev = now;
    sHave = true;
}

// Is row `i` the selected one? The one place that answers it, because it is asked from
// more than one.
bool RowIsSelected(int i) {
    return !g_selectedId.empty() && i >= 0 && i < static_cast<int>(g_rowIds.size()) &&
           g_rowIds[static_cast<size_t>(i)] == g_selectedId;
}

// Repaint the rows whose SELECTION STATE CHANGED -- ALL THREE CHANNELS, and never more
// than two rows.
//
// IT REPAINTS THE TEXT, AND THE FIRST VERSION DID NOT. That version repainted the fill and
// the frame and stopped, on the reasoning -- written out in this very spot -- that a click
// lands on the row the pointer is ON, so the newly selected row is by construction the
// hovered one and would otherwise be left purple AND yellow-framed. Every word of that is
// true of the THIRD channel too, and it did not carry: hover paints the frame and the FIVE
// TEXT BLOCKS together, so a fill+frame repaint left the just-clicked row purple with a
// grey frame and YELLOW GLYPHS -- the exact combination `PointerLit` exists to forbid,
// visible on every single click, until the pointer moved off the row or the next 5 s fetch
// happened to land. Caught by a post-ship audit, 2026-08-30; neither self-check shot could
// see it, because both of them move the cursor first and the move heals it.
//
// The lesson one level up, and the reason this is now a PAIR walk rather than a full one:
// "repaint the changed state" is only correct if it enumerates every channel that state
// drives. Two of three is not a partial fix, it is a new inconsistency.
//
// TWO ROWS, NOT ALL OF THEM. Only the row that lost the selection and the row that gained
// it changed anything; every other row's answer to `PointerLit` and to `selected` is what
// it already was. The scan over `g_rowIds` is pure string compares and costs no dispatch,
// so this is ~30 dispatches instead of the ~380 a full-list skin walk cost -- cheaper than
// the version it replaces AND correct, which is why `RowSkinAt` (a cut-down parts reader
// that only the full walk needed) is retired with it rather than kept for a caller that no
// longer exists.
void RepaintSelectionChange(const std::string& wasId) {
    if (wasId == g_selectedId) return;   // nothing changed hands
    // Hoisted: it takes a mutex and copies a string, and this loop can body twice.
    const std::string own = sm::OwnLobbyId();
    const int total = g_visibleRows;   // shown, not ChildCount's high-water mark
    for (int i = 0; i < total && i < static_cast<int>(g_rowIds.size()); ++i) {
        const std::string& id = g_rowIds[static_cast<size_t>(i)];
        if (id.empty() || (id != wasId && id != g_selectedId)) continue;
        if (i >= static_cast<int>(g_rows.size())) continue;
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        const Row& r = g_rows[static_cast<size_t>(i)];
        bool mismatch = false;
        (void)VersionCell(r, mismatch);
        const bool sel = (id == g_selectedId);
        ApplyRowSkin(rp, i == g_hoverRow, sel);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, mismatch,
                           i == g_hoverRow, sel);
    }
}

}  // namespace

void BuildHeader(void* parent) {
    // Column headers are a SECTION HEADER, and those are orange in every native window.
    void* head = Spawn(L"HorizontalBox", parent);
    if (head) {
        for (const Column& c : kColumns) AddText(head, c.title, 16, kAccent, kJustLeft, c.weight);
        U::AddChild(parent, head);
    }
}

void Attach(void* listPanel) {
    g_list = listPanel;
    g_rowIds.clear();
}

void* Panel() { return g_list; }

void OnShown() {
    g_hover.Reset();
    g_hoverRow = -1;   // Sync paints THIS row hovered, so a stale one survives the reset
}

uint64_t PaintedGeneration() { return g_lastRowsGen; }

// WHICH ROW IS UNDER THE CURSOR, re-evaluated only when something could have changed it.
//
// IT DOES NOT ASK SLATE, AT EITHER LEVEL. This used to read "there is no cheaper way to
// ask: FGeometry has no reflected members (size 0x38, empty), so the rows' screen rects
// are not readable", and gate a per-row `IsHovered` walk on `IsHovered(g_list)`. Every
// part of that was wrong: the rects ARE readable (through USlateBlueprintLibrary, never
// through FGeometry's members), the outer gate is FALSE while the cursor is genuinely over
// the list, and the row backgrounds do not answer either -- all three measured 2026-08-29.
// The whole hit test now lives in `native_screen::ChildAtCursor`, which carries the
// measurement, and the hosting window's world list uses the same one.
//
// The consequence of the old shape was total rather than partial: the gate never opened,
// so no row highlighted and `g_hoverRow` stayed -1 -- which is also what the click path
// reads, so no server could be selected either.
void UpdateHover() {
    // The tracker owns the pointer, the scroll and the settling pass together; this screen
    // used to own all three itself, and its sibling owned a broken copy of them.
    if (!g_hover.Poll(g_list, g_visibleRows)) return;
    const int hit = g_hover.Index();
    if (hit == g_hoverRow) return;

    const int prev = g_hoverRow;
    g_hoverRow = hit;
    // Hoisted: it takes a mutex and copies a string, and the loop below runs it twice.
    const std::string own = sm::OwnLobbyId();
    // Edge-applied: only the two rows that changed are repainted -- the one the pointer
    // left and the one it arrived on. Both channels the pointer owns, the frame and the
    // text, so a row cannot keep a yellow frame after the cursor has gone.
    for (int i : {prev, hit}) {
        if (i < 0 || i >= static_cast<int>(g_rows.size())) continue;
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        const Row& r = g_rows[static_cast<size_t>(i)];
        bool mismatch = false;
        (void)VersionCell(r, mismatch);
        const bool sel = RowIsSelected(i);
        ApplyRowSkin(rp, i == hit, sel);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, mismatch, i == hit, sel);
    }
}

// THE SINGLE WRITER of both the children's text and g_rowIds. Nothing else touches either,
// which is what makes the positional pairing safe (header invariant).
void Sync() {
    g_lastRowsGen = sm::CopyRows(g_rows);
    const int want = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows
                                                               : static_cast<int>(g_rows.size());
    // THE TRUNCATION IS NO LONGER SILENT. `kMaxRows` bounds the whole sync loop, so a
    // master serving 100 lobbies renders 64 and the player has no way to learn that the
    // server they are looking for is one of the 36 that were dropped. Logged on a CHANGE,
    // not per sync, so a steady over-cap list costs one line and not one per 5 seconds.
    //
    // The cap itself is NOT raised here. Raising it is step T2b and it wants the baseline
    // T2c takes against it; what does not need a measurement is admitting when it bites.
    {
        static int sLastTruncated = 0;
        const int dropped = static_cast<int>(g_rows.size()) - want;
        if (dropped != sLastTruncated) {
            sLastTruncated = dropped;
            if (dropped > 0)
                UE_LOGW("server_browser_rows: the master listed %d servers and the list "
                        "renders %d -- %d are NOT SHOWN (kMaxRows). Raising the cap is "
                        "MULTIPLAYER_UI section 8c.-1 step T2b, which wants a baseline "
                        "first", static_cast<int>(g_rows.size()), want, dropped);
        }
    }
    // SCROLL POSITION SURVIVES A STRUCTURAL CHANGE.
    //
    // Rows are grown and collapsed as the list changes size, which changes the content
    // height under a view that is holding an offset -- so a lobby appearing or vanishing
    // while the player is scrolled down moves the rows they were reading. Saved before the
    // change and written back after, and ONLY when the shown count actually changes, so a
    // sync that merely rewrites the same rows costs nothing.
    //
    // `GetScrollOffset` reports Slate's DesiredScrollOffset -- what was last ASKED FOR. A
    // wheel scroll goes through that same field, so it is the right value to CARRY; what it
    // is not is a value that can be written back unchecked. See the clamp at the restore.
    const int  hadRows    = g_visibleRows;   // read BEFORE the assignment below overwrites it
    const bool structural = (want != hadRows);
    float keepOffset = 0.f;
    const bool haveOffset = structural && hadRows > 0 && U::ScrollOffset(g_list, keepOffset);
    if (structural && hadRows > 0 && !haveOffset)
        UE_LOGW("server_browser_rows: rows %d -> %d but the scroll offset would not read "
                "-- the view drops to the top rather than holding position", hadRows, want);
    // The hover walk reads this instead of ChildCount: rows are GROWN and never
    // removed (a surplus row is Collapsed below, not destroyed), so ChildCount is a
    // HIGH-WATER MARK -- once the list has held 64 it reports 64 forever, and the walk
    // would read the geometry of rows that are not on screen. A collapsed widget is not
    // arranged, so it keeps its last painted rect, which can still contain the cursor
    // and win the hover shortcut over the real row beneath it.
    int have = U::ChildCount(g_list);
    if (have < 0) have = 0;
    // GROW by building; SHRINK by COLLAPSING, never by detaching. Nothing roots a detached
    // row -- the panel's Slots is the only reference -- so a detached free-list would be a
    // use-after-free waiting for the next GC. A collapsed row stays rooted, is not
    // hit-testable, and a regrow just un-collapses it.
    for (int i = have; i < want; ++i) {
        void* row = BuildRow(g_list);
        if (!row) break;
        if (void* s = U::AddChild(g_list, row)) {
            U::SetSlotAlign(s, P::off::UScrollBoxSlot_HAlign, P::off::UScrollBoxSlot_VAlign,
                            kFill, kCenter);
            // ROW SEPARATION WITHOUT A PER-ROW BORDER, and this is a stated trade. Native
            // rows each carry their own frame; reproducing that costs one more UImage per
            // row, and this list's per-row cost is the exact subject of the open perf lane
            // (MULTIPLAYER_UI section 8c.-1). A gap that lets the darker panel show through
            // reads as a boxed row at zero extra widgets. If T6 lands a viewport pool, the
            // widget budget stops mattering and the real frame can replace this.
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UScrollBoxSlot_Padding);
            pad[0] = 0.f; pad[1] = 0.f; pad[2] = 0.f; pad[3] = kRowGapPx;
        }
    }
    // What the pointer was on BEFORE this pass rewrote the ids -- see the invalidation
    // check after the loop.
    const std::string hoveredIdBefore =
        (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_rowIds.size()))
            ? g_rowIds[static_cast<size_t>(g_hoverRow)] : std::string();
    const int total = U::ChildCount(g_list);
    // AFTER the grow loop, and clamped to what exists. The grow loop can `break` when
    // BuildRow fails, so assigning `want` up front could promise more shown rows than
    // there are children -- and the hover walk and the fill repaint both iterate this.
    // `native_screen.h` states the contract as 'children actually SHOWN'; over-reporting
    // breaks it from the other side, exactly as ChildCount broke it from the first.
    g_visibleRows = (total >= 0 && total < want) ? total : want;
    g_rowIds.assign(total < 0 ? 0 : total, std::string());

    const std::string own = sm::OwnLobbyId();
    for (int i = 0; i < total; ++i) {
        // THE SURPLUS BRANCH COMES FIRST. A row past `want` needs exactly one thing --
        // its box, to collapse it -- and deriving all seven parts to get it cost ten
        // dispatches per surplus row on every sync, forever, for rows nobody can see.
        // Rows are grown and never removed, so on a list that has ever been long the
        // surplus is most of the loop. (Post-ship perf audit 2026-08-30, F1.)
        if (i >= want) {
            if (void* box = U::ChildAt(g_list, i))
                E::SetWidgetVisibility(box, 1);  // ESlateVisibility::Collapsed
            continue;
        }
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        E::SetWidgetVisibility(rp.box, 0);
        const Row& r = g_rows[static_cast<size_t>(i)];
        const bool isOwn = !own.empty() && r.lobbyId == own;
        bool mismatch = false;
        const std::wstring ver = VersionCell(r, mismatch);

        // THE LOCK MARKER, and it says the same letter the other surface says.
        //
        // A locked lobby rendered identically to an open one is not a cosmetic gap: a
        // player picks it, waits through a connect, and is refused -- and the browser gave
        // them nothing to have chosen differently on. The ImGui browser has drawn an "L"
        // in its first column since it was written; the native one drew nothing, which the
        // parity gate (tools/ui/browser_parity_gate.py) now fails on.
        //
        // It is a NAME PREFIX rather than a sixth column because the row is five text
        // blocks wide by construction and "(your server)" already establishes the idiom of
        // qualifying the name in place. Same letter as the fallback deliberately: two
        // surfaces describing one lobby differently is the drift this lane exists to end.
        SetRowText(rp.text[0],
                   std::string(r.locked ? "L  " : "") +
                   (isOwn ? r.name + "   (your server)" : r.name));
        SetRowText(rp.text[1], std::to_string(r.playersCur) + "/" + std::to_string(r.playersMax));
        if (rp.text[2]) E::SetWidgetText(rp.text[2], ver.c_str());
        SetRowText(rp.text[3], r.world);
        SetRowText(rp.text[4], std::to_string(r.ageSec) + "s");
        // SELECTION IS KEYED ON THE LOBBY, NOT THE ROW INDEX. The client now sorts, so the
        // order is stable for a given SET -- but the SET churns, and one host leaving
        // shifts every row after it. A refresh can therefore still put a different server
        // at index N, and an index-keyed selection would silently move to whatever landed
        // there, which is how a player joins a server they did not pick.
        //
        // Read from `r.lobbyId` and NOT through `RowIsSelected(i)`: that helper reads
        // `g_rowIds[i]`, and the `assign` above CLEARED every entry to an empty string
        // before this loop began refilling them -- so the helper would answer `false` for
        // every row and no selection would ever paint. (The first version of this comment
        // said it 'still holds the PREVIOUS pass's id', which is the wrong mechanism for
        // the right conclusion; a post-ship audit read the `assign` and caught it.)
        const bool sel = !g_selectedId.empty() && r.lobbyId == g_selectedId;
        ApplyRowSkin(rp, i == g_hoverRow, sel);
        ApplyRowTextColors(rp, r, isOwn, mismatch, i == g_hoverRow, sel);
        // The id is captured HERE, in the same pass as the text above it, so a click
        // resolves to the server the user was LOOKING at even if the master reorders.
        g_rowIds[static_cast<size_t>(i)] = r.lobbyId;
    }

    // THE PAINT INVALIDATES THE HOVER, AND NOTHING ELSE CAN SEE THAT.
    //
    // `HoverTracker` re-evaluates when the pointer moves, the list scrolls, or the count
    // changes. A re-sync that keeps the SAME count while the membership shifts is none of
    // those, and the tick order is ClickSelect -> UpdateHover -> Sync, so a sync at tick k
    // rewrites g_rowIds AFTER that tick's hover was decided -- and the click at k+1 reads
    // g_rowIds[g_hoverRow], now naming a different server. Narrow (one frame, and a fetch
    // must land in it) and still the exact wrong-server-selected defect the id pairing
    // exists to prevent.
    //
    // This is the project's own lesson one level up: `HoverTracker` was extracted so the
    // hit test had one owner, and the OTHER thing that invalidates a hover -- the repaint
    // -- stayed behind. Dropping the index is right rather than re-deriving it: the
    // pointer has not moved, so the next real motion answers correctly, and until then a
    // click selects nothing instead of selecting the wrong thing.
    if (g_hoverRow >= 0) {
        const bool gone = g_hoverRow >= g_visibleRows ||
                          g_hoverRow >= static_cast<int>(g_rowIds.size()) ||
                          g_rowIds[static_cast<size_t>(g_hoverRow)] != hoveredIdBefore;
        if (gone) {
            g_hoverRow = -1;
            g_hover.Reset();   // or the tracker reports 'unchanged' and never re-asks
        }
    }

    // Every id this pass rendered is now in g_rowIds, so the order invariant can be checked
    // against what is actually ON SCREEN rather than against the network list.
    CheckOrderStable(want);

    // Put the view back where the player left it. The log line is the MEASUREMENT this
    // ships with: nothing has ever observed whether a grow/collapse actually moves this
    // box, so the restore reports the one case that proves it was needed -- a structural
    // change while the list was scrolled away from the top.
    if (haveOffset && keepOffset > 0.f) {
        // CLAMPED, AND THE CLAMP IS THE WHOLE CORRECTNESS OF THIS.
        //
        // `GetScrollOffset` ECHOES the last request and applies NO clamp of its own
        // (`umg_build.h:159-171`, measured twice on 2026-08-26: asked for 1000000 it
        // returns 1000000, on an empty box and on one with 1391 units of real overflow).
        // So writing back an offset the SHRUNKEN list cannot honour does not merely
        // over-scroll for a frame -- it STORES a number that the next structural change
        // reads back as though it were a real position, and the view then jumps that far
        // away from a player who was reading the top. An earlier version of this comment
        // asserted the layout would clamp it for us; that is exactly the claim the kit's
        // own header had already measured false, and the post-ship audit caught it.
        //
        // The bound is COMPUTED, not read back: `GetScrollOffsetOfEnd` reports the extent
        // Slate arranged LAST frame, and this runs in the same tick as the visibility
        // writes that change it. The rows' own metrics are the honest source -- the module
        // that decides how tall a row is can say how far a list of them scrolls.
        ue_wrap::FVector2D ltl{}, lsz{};
        float target = keepOffset;
        if (U::WidgetScreenRect(g_list, ltl, lsz) && lsz.Y > 0.f) {
            const float content = static_cast<float>(want) * (kRowH + kRowGapPx);
            const float maxOff  = content > lsz.Y ? content - lsz.Y : 0.f;
            if (target > maxOff) target = maxOff;
        }
        if (target > 0.f) {
            U::SetScrollOffset(g_list, target);
            if (target < keepOffset)
                UE_LOGI("server_browser_rows: rows %d -> %d while scrolled to %.1f -- "
                        "offset restored, CLAMPED to %.1f (the shorter list cannot hold "
                        "the old position)", hadRows, want, keepOffset, target);
            else
                UE_LOGI("server_browser_rows: rows %d -> %d while scrolled to %.1f -- "
                        "offset restored", hadRows, want, keepOffset);
        } else {
            // The new list fits entirely in the viewport: there is no position to hold,
            // and writing 0 is what "back to the top" means.
            U::SetScrollOffset(g_list, 0.f);
            UE_LOGI("server_browser_rows: rows %d -> %d while scrolled to %.1f -- the "
                    "shorter list no longer overflows, so the view returns to the top",
                    hadRows, want, keepOffset);
        }
    }
}

bool ClickSelect() {
    // A click on a hovered row SELECTS it. The row under the cursor is already known from
    // the hover pass, so this costs no extra dispatch.
    if (g_hoverRow < 0 || g_hoverRow >= static_cast<int>(g_rowIds.size())) return false;
    const std::string& id = g_rowIds[static_cast<size_t>(g_hoverRow)];
    if (id.empty() || id == g_selectedId) return false;
    const std::string was = g_selectedId;   // read BEFORE the assignment; the repaint needs it
    g_selectedId = id;
    RepaintSelectionChange(was);
    UE_LOGI("server_browser_rows: row selected (%s)", id.c_str());
    return true;
}

int HoveredRow() { return g_hoverRow; }

const char* SelectedId() { return g_selectedId.c_str(); }

bool Selected(coop::net::lobby::LobbyRow& out) {
    if (g_selectedId.empty()) return false;
    // BY ID, never by index -- this module's header invariant. `g_rows` is refreshed from
    // the master on a timer and its MEMBERSHIP changes, so the position that was selected is
    // not necessarily the position that holds it now -- the sort fixes the order, not the
    // set -- and a positional read would hand a player a different server than the one they
    // clicked.
    for (const Row& r : g_rows) {
        if (r.lobbyId == g_selectedId) { out = r; return true; }
    }
    // Selected, but the lobby is gone from the latest list: the host quit while the screen
    // was open. Answering false is right -- the caller has nothing to connect to -- and the
    // selection is dropped so the highlight stops pointing at a server that is not there.
    const std::string was = g_selectedId;
    g_selectedId.clear();
    RepaintSelectionChange(was);
    return false;
}

void LogRowHitDiagnostics(int32_t i) {
    RowParts rp;
    if (!RowPartsAt(i, rp)) {
        UE_LOGW("server_browser_rows: row %d has no parts -- RowPartsAt failed, so it has "
                "no background to tint and no text to recolour", i);
        return;
    }
    auto dump = [](const char* what, void* w) {
        if (!w) { UE_LOGW("server_browser_rows:   %s is NULL", what); return; }
        ue_wrap::FVector2D tl{}, sz{};
        const bool haveRect = U::WidgetScreenRect(w, tl, sz);
        UE_LOGW("server_browser_rows:   %s hovered=%d rect %s(%.0f,%.0f) %.0fx%.0f",
                what, E::WidgetIsHovered(w) ? 1 : 0, haveRect ? "" : "UNREAD ",
                tl.X, tl.Y, sz.X, sz.Y);
        U::LogVisibilityChain(what, w);
    };
    dump("row.edge", rp.edge);
    dump("row.face", rp.face);
    dump("row.text0", rp.text[0]);
}

}  // namespace ui::server_browser_rows
