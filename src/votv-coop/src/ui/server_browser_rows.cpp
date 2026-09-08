// ui/server_browser_rows.cpp -- the server list's rows: build, paint, hover, selection and the
// sync from the fetched lobby list (the header states the id-pairing invariant).

#include "ui/server_browser_rows.h"

#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"       // the one owner of text encoding
#include "ui/native_screen.h"           // palette + widget primitives, shared with the host window
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>   // GetTickCount64 -- how long ago the rows we hold were fetched

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
using NS::kJustRight;
using NS::AddText;
using NS::Spawn;

// Slate units; the row height is the game's own (uicomp_saveSlot_C is 64 px).
constexpr float kRowH     = 64.f;
constexpr float kRowGapPx = 2.f;
// Every row carries its own frame (docs/votv-ui-style.md: nothing floats unboxed), inside the
// 64 px row so the scroll arithmetic is unchanged; the 2 px slot gap stays, or two adjacent frames
// read as one 4 px rule.
constexpr float kRowBorderPx = 2.f;
// Bounds the whole sync loop, not only the display: `want` clamps to it and the grow loop to
// `want`. The master emits every listed lobby, so an over-cap list is truncated (Sync logs it on
// change). Raising it is priced at ~28 ProcessEvent dispatches per painted row, all in one frame,
// and waits for a measured baseline.
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
const FLinearColor kBad    = NS::Bad();     // #FF0000 -- the join gate will refuse this one
// A stale row is the same colours at 40% alpha, never a different hue (MTA's bMaybeOffline dims
// rather than recolours): a confidence, not a category.
FLinearColor Faded(const FLinearColor& c) { return FLinearColor{c.R, c.G, c.B, 0.40f}; }

// ---- state (game thread only) ----
void* g_list = nullptr;   // the UScrollBox holding the rows

// Hover and selection are different channels, and selection outranks hover. Native hover is a
// text colour change to #FFFF00 and native selection a row fill change to #400040 (the style doc,
// measured); here hover also yellows the frame, since a 640 px data row's glyphs alone are a
// change the eye misses, and a selected row ignores hover entirely. Hover owns the frame and the
// text, selection owns the fill; where both apply, PointerLit suppresses hover at the source.
int              g_hoverRow = -1;   // index into the live rows, or -1
NS::HoverTracker g_hover;           // pointer + scroll + settling, one owner
std::string      g_selectedId;      // the SELECTED LOBBY, keyed by id and not by index

// Row identity: g_rowIds[i] is the lobbyId the child at index i was rendered with, written in the
// same pass as its text by the one function that owns the panel.
std::vector<std::string> g_rowIds;
std::vector<Row> g_rows;
uint64_t g_lastRowsGen  = 0;   // the fetch generation last PAINTED
int      g_visibleRows  = 0;   // rows actually SHOWN, not ChildCount's high-water mark
// When the rows we hold were fetched, in local ticks: `ageSec` is the master's seconds since
// heartbeat at that moment, so the true age is ageSec + (now - this). Stamped only when the data
// generation moves.
uint64_t g_fetchedAtMs  = 0;
uint64_t g_lastDataGen  = 0;   // the DATA generation the stamp above was taken at

// Three cells, not five columns, and no header strip: the game's save browser gives each row a
// title and one right-hand fact and puts every other detail in a side panel, so the name takes
// the row, "Players: c/m" sits at the right, and the middle cell is a red mismatch mark (its own
// cell because a UTextBlock has one colour). The weights are measured against the longest string
// each cell holds: a right-aligned cell clips at the left, and 0.24 clipped "Players: 2/4".
struct Cell { float weight; };
constexpr Cell kCells[3] = {
    {0.58f},   // name        -- accent orange, left
    {0.14f},   // mismatch    -- #FF0000, left, EMPTY unless the join gate would refuse
    {0.28f},   // "Players: n/m" -- white, right
};
constexpr int kCellName = 0, kCellFlag = 1, kCellPlayers = 2;

// A row goes dim when its host has stopped checking in: the master reaps a silent host at a
// measured 111-116 s (three 30 s heartbeats plus slack), so one missed cycle means a server that
// is probably gone. Dimmed, not removed (MTA's CServerListItem does the same): removing rows
// under a reading hand is worse.
constexpr int kStaleSec = 65;

// AgeNowSec is public (the details panel reads it too) and defined below; this is the painters'
// predicate.
bool IsStale(const Row& r) { return AgeNowSec(r) > kStaleSec; }

// A padlock drawn from four tinted rects (no asset, no font, no donor): the game's font_ui is a
// pixel monospace face with no lock glyph, and the tree has no lock texture. A UImage with a tint
// and no ResourceObject is a solid rect, and an Overlay slot with Fill alignment plus asymmetric
// padding positions it, so each bar is one widget; at 18 px it reads as a padlock (MTA reserves a
// 16 px icon cell for the same job). The overlay is returned so the caller can tint the glyph.
constexpr float kLockCell = 18.f;

void* AddLockGlyph(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    void* ovl = box ? Spawn(L"Overlay", box) : nullptr;
    if (!box || !ovl) return nullptr;
    U::SetSizeBoxWidth(box, kLockCell);
    U::SetSizeBoxHeight(box, kLockCell);

    // {left, top, right, bottom} insets inside the 18x18 cell: shackle left, shackle right, shackle
    // top, body.
    const float kBars[4][4] = {
        { 5.f,  4.f, 11.f,  8.f},   // upright, left
        {11.f,  4.f,  5.f,  8.f},   // upright, right
        { 5.f,  3.f,  5.f, 13.f},   // the top of the shackle, joining them
        { 3.f,  9.f,  3.f,  2.f},   // the body
    };
    for (const auto& b : kBars) {
        void* img = Spawn(L"Image", ovl);
        if (!img) return nullptr;
        U::SetImageTintRaw(img, kDim);
        E::SetWidgetVisibility(img, 3);   // HitTestInvisible: it draws, never eats a click
        if (void* s = U::AddChild(ovl, img)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            kFill, kFill);
            NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, b[0], b[1], b[2], b[3]);
        }
    }
    U::SetContent(box, ovl);
    // Attach it: Spawn takes `parent` as the UObject outer and does not add the widget. RowPartsAt
    // reads the text cells by index, so an unattached first child shifts every one of them. Weight
    // 0 = Automatic, so the SizeBox's own 18 px is honoured.
    if (void* s = NS::AddHFill(parent, box, 0.f, kCenter, kCenter))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f, 8.f, 0.f);
    return box;
}

// One row: USizeBox(64) -> UOverlay (the kit's AddFramedBox: edge, face) -> UHorizontalBox of the
// lock cell and three texts. No UButton: the native row's own button_select draws nothing in all
// three states, and a UButton would add a press visual to suppress. The frame images are
// HitTestInvisible; the hit test reads the SizeBox's rect (native_screen::ChildAtCursor).
void* BuildRow(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    if (!box) return nullptr;
    U::SetSizeBoxHeight(box, kRowH);
    void* ovl = NS::AddFramedBox(box, kRowBg, kRowBorderPx);
    void* hb  = ovl ? Spawn(L"HorizontalBox", ovl) : nullptr;
    if (!ovl || !hb) return nullptr;
    if (void* s = U::AddChild(ovl, hb)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        // The row's inner gutter, left only: every weighted cell already carries AddText's 18 px
        // right gutter, so a right pad would double-inset the player count.
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, 10.f, 0.f, 0.f, 0.f);
    }
    // The lock cell comes first and is present on every row (hidden when the lobby is open): a cell
    // only locked rows have would shift the name left and right down the list.
    AddLockGlyph(hb);
    // Sizes and colours are the cell's: the name is the row's title (larger, accent), the two facts
    // beside it body text.
    AddText(hb, L"", 20, kAccent, kJustLeft,  kCells[kCellName].weight);
    AddText(hb, L"", 16, kBad,    kJustLeft,  kCells[kCellFlag].weight);
    AddText(hb, L"", 16, kText,   kJustRight, kCells[kCellPlayers].weight);
    // SizeBox is a UContentWidget: its single child goes through SetContent.
    U::SetContent(box, ovl);
    return box;
}

// A row's parts, re-derived from the panel on demand: no row pointers are held across ticks
// (cached_obj_ref.h: the world stamp is inert for UMG, and a hand-spawned widget captures serial
// 0). The panel's Slots is the authority.
struct RowParts { void* box; void* edge; void* face; void* lock; void* text[3]; };
bool RowPartsAt(int32_t i, RowParts& out) {
    out = RowParts{};
    void* box = U::ChildAt(g_list, i);
    if (!box) return false;
    out.box = box;
    void* ovl = nullptr;
    // Latched once, including on failure: FindFunction has no result cache and walks the whole
    // GUObjectArray, and this runs per row inside Sync. A UFunction either exists at process start
    // or never does, so a permanent negative latch is correct.
    static void* const sGetContent = [] {
        void* cw = R::FindClass(P::name::ContentWidgetClass);
        return cw ? R::FindFunction(cw, P::name::GetContentFn) : nullptr;
    }();
    if (sGetContent) {
        ue_wrap::ParamFrame f(sGetContent);
        if (Call(box, f)) ovl = f.Get<void*>(L"ReturnValue");
    }
    if (!ovl) return false;
    // The kit owns its child order: reading slots by literal index broke when the native-material
    // frame reordered them, and UPanelWidget::Slots and UImage::Brush sit at the same offset, so
    // GetChildAt on the wrong child reads a brush vtable as a slot array.
    NS::FramedParts fp;
    if (!NS::FramedBoxParts(ovl, fp)) return false;
    out.edge = fp.edge;
    out.face = fp.face;
    void* hb = fp.content;
    if (!hb) return false;
    // Child 0 of the text row is the lock cell; the three text cells follow it.
    out.lock = U::ChildAt(hb, 0);
    for (int c = 0; c < 3; ++c) out.text[c] = U::ChildAt(hb, c + 1);
    return true;
}

// Will the join gate refuse this server, and which half of the pair disagrees: `game` is the
// host's VOTV target and `proto` its build. The row shows the shortest true thing ("!b146" when
// only the build differs, "!0.9.0m" when the cook does); the details panel spells out both sides.
std::string MismatchMark(const Row& r) {
    const bool gameBad = !r.game.empty() && r.game != sm::GameTarget();
    const bool protoBad = r.proto > 0 && r.proto != static_cast<int>(coop::net::kProtocolVersion);
    if (gameBad) return "!" + r.game;
    if (protoBad) return "!b" + std::to_string(r.proto);
    return {};
}

// Does the pointer get to change this row at all: a selected row is deaf to hover in every
// channel. Written once so the three painters cannot disagree.
bool PointerLit(bool hovered, bool selected) { return hovered && !selected; }

// A row's two fill channels, one owner: the fill says chosen (#400040, set by a click), the frame
// says pointed at (#FFFF00, transient) and falls back to the #646464 every native box carries.
// Independent writes to independent widgets; the precedence lives in PointerLit.
void ApplyRowSkin(const RowParts& rp, bool hovered, bool selected) {
    // SetImageTint, not the raw write: these images are attached to Slate, and a raw property write
    // does not repaint.
    if (rp.face) U::SetImageTint(rp.face, selected ? kRowSel : kRowBg);
    if (rp.edge) U::SetImageTint(rp.edge, PointerLit(hovered, selected) ? kHover : kBorder);
}

// A row's three text colours, one owner, so un-hovering cannot restore the wrong base.
void ApplyRowTextColors(const RowParts& rp, const Row& r, bool isOwn, bool stale,
                        bool hovered, bool selected) {
    // Hovering turns the label yellow and leaves the fill alone; a selected row keeps its data
    // colours on the purple, and the pointer does not override that.
    const bool lit = PointerLit(hovered, selected);
    // The name carries the accent, green for our own lobby, yellow under the pointer. The mismatch
    // mark stays red under the pointer: yellowing it would erase the one thing it says.
    FLinearColor name = lit ? kHover : (isOwn ? kOwn : kAccent);
    FLinearColor body = lit ? kHover : kText;
    FLinearColor flag = kBad;
    // Stale last, so it fades whatever the state above chose.
    if (stale) { name = Faded(name); body = Faded(body); flag = Faded(flag); }
    // The dispatch variant, never the raw write: UMG bakes the property into the Slate widget at
    // attach, so a raw write to a block in a constructed tree never reaches the screen (only the
    // WidgetComponent nameplates re-render from properties). Column headers coloured at build time,
    // before attach, are why a screen with dead colour writes once looked right.
    if (rp.text[kCellName])    E::SetTextBlockColorDispatch(rp.text[kCellName], name);
    if (rp.text[kCellFlag])    E::SetTextBlockColorDispatch(rp.text[kCellFlag], flag);
    if (rp.text[kCellPlayers]) E::SetTextBlockColorDispatch(rp.text[kCellPlayers], body);
    (void)r;
}

// Decoded, not widened: std::wstring(s.begin(), s.end()) copies each UTF-8 byte into a wchar and
// renders a Cyrillic name as one garbage glyph per byte. The lossy decode is right here: a row
// must still draw when a hostile master sends ill-formed bytes, and refusing the field is the
// receive boundary's job. Text only; the colour has one owner, ApplyRowTextColors.
void SetRowText(void* block, const std::string& utf8) {
    if (!block) return;
    const std::wstring w = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
    E::SetWidgetText(block, w.c_str());
}

// The order invariant, watched rather than assumed: the list is sorted at its one producer
// (lobby_client.cpp, at the parse site), a property this file cannot see, and a permuted list
// looks exactly like a changed one. Two digests of what was rendered, one over the id sequence
// and one over the id set: the same set with a different sequence is the defect. Blind above
// kMaxRows, where the visible set is a window onto a longer list.
struct OrderDigest { uint64_t sequence = 0; uint64_t set = 0; };

OrderDigest DigestIds(const std::vector<std::string>& ids, int count) {
    OrderDigest d;
    d.sequence = 1469598103934665603ull;   // FNV-1a offset basis
    for (int i = 0; i < count && i < static_cast<int>(ids.size()); ++i) {
        // The index is mixed into the set term: two rows whose RowPartsAt failed both carry an
        // empty id, and two identical hashes XOR to zero, which would silently erase them from the
        // digest. The sequence term is positional by construction.
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

// Is row `i` the selected one? The one place that answers it.
bool RowIsSelected(int i) {
    return !g_selectedId.empty() && i >= 0 && i < static_cast<int>(g_rowIds.size()) &&
           g_rowIds[static_cast<size_t>(i)] == g_selectedId;
}

// Repaint the rows whose selection state changed: all three channels (hover paints the frame and
// the text together, so a fill-plus-frame repaint leaves the just-clicked row purple with yellow
// glyphs), and only the two rows that changed hands (~30 dispatches instead of a ~380 full-list
// walk).
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
        const bool sel = (id == g_selectedId);
        ApplyRowSkin(rp, i == g_hoverRow, sel);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, IsStale(r),
                           i == g_hoverRow, sel);
    }
}

}  // namespace

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

int Count() { return static_cast<int>(g_rows.size()); }

uint64_t MsSinceFetch() {
    if (g_fetchedAtMs == 0) return 0;
    const uint64_t now = ::GetTickCount64();
    return now > g_fetchedAtMs ? now - g_fetchedAtMs : 0;
}

int AgeNowSec(const coop::net::lobby::LobbyRow& r) {
    if (g_fetchedAtMs == 0) return r.ageSec;
    return r.ageSec + static_cast<int>(MsSinceFetch() / 1000u);
}

// Which row is under the cursor, re-evaluated only when something could have changed it. The hit
// test lives in native_screen::ChildAtCursor (the rows' rects read through USlateBlueprintLibrary;
// Slate's IsHovered answers false on the list and on the row images, measured), shared with the
// hosting window's world list.
void UpdateHover() {
    // The tracker owns the pointer, the scroll and the settling pass together.
    if (!g_hover.Poll(g_list, g_visibleRows)) return;
    const int hit = g_hover.Index();
    if (hit == g_hoverRow) return;

    const int prev = g_hoverRow;
    g_hoverRow = hit;
    // Hoisted: it takes a mutex and copies a string, and the loop below runs it twice.
    const std::string own = sm::OwnLobbyId();
    // Edge-applied: only the row the pointer left and the one it arrived on, in both channels the
    // pointer owns, so a row cannot keep a yellow frame after the cursor has gone.
    for (int i : {prev, hit}) {
        if (i < 0 || i >= static_cast<int>(g_rows.size())) continue;
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        const Row& r = g_rows[static_cast<size_t>(i)];
        const bool sel = RowIsSelected(i);
        ApplyRowSkin(rp, i == hit, sel);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, IsStale(r), i == hit, sel);
    }
}

// The single writer of both the children's text and g_rowIds, which is what makes the positional
// pairing safe.
void Sync() {
    g_lastRowsGen = sm::CopyRows(g_rows);
    // A successful fetch resets the age clock; a repaint or a failed fetch does not. Keyed on the
    // data generation, not the repaint generation, which also moves on a failed attempt (the status
    // line changed): a down master would otherwise re-stamp "last fetched" every 5 s and the stale
    // dim could never fire.
    const uint64_t dataGen = sm::RowsDataGeneration();
    if (dataGen != g_lastDataGen || g_fetchedAtMs == 0) {
        g_lastDataGen = dataGen;
        g_fetchedAtMs = ::GetTickCount64();
    }
    const int want = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows
                                                               : static_cast<int>(g_rows.size());
    // The truncation is not silent: a master serving 100 lobbies renders 64, and the player has no
    // other way to learn the one they want is among the dropped. Logged on change, not per sync.
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
    // Scroll position survives a structural change: a lobby appearing or vanishing while the player
    // is scrolled down would move the rows they were reading. Saved before, restored after, only
    // when the shown count changes. GetScrollOffset echoes the last request (a wheel scroll goes
    // through the same field), so it is the value to carry, but not one to write back unchecked.
    const int  hadRows    = g_visibleRows;   // read BEFORE the assignment below overwrites it
    const bool structural = (want != hadRows);
    float keepOffset = 0.f;
    const bool haveOffset = structural && hadRows > 0 && U::ScrollOffset(g_list, keepOffset);
    if (structural && hadRows > 0 && !haveOffset)
        UE_LOGW("server_browser_rows: rows %d -> %d but the scroll offset would not read "
                "-- the view drops to the top rather than holding position", hadRows, want);
    // The hover walk reads g_visibleRows, not ChildCount: rows are grown and never removed, so
    // ChildCount is a high-water mark, and a collapsed row keeps its last painted rect, which can
    // still contain the cursor.
    int have = U::ChildCount(g_list);
    if (have < 0) have = 0;
    // Grow by building, shrink by collapsing, never by detaching: nothing roots a detached row (the
    // panel's Slots is the only reference), so a detached free-list would be a use-after-free at
    // the next GC.
    for (int i = have; i < want; ++i) {
        void* row = BuildRow(g_list);
        if (!row) break;
        if (void* s = U::AddChild(g_list, row)) {
            U::SetSlotAlign(s, P::off::UScrollBoxSlot_HAlign, P::off::UScrollBoxSlot_VAlign,
                            kFill, kCenter);
            // The slot gap below each row: two adjacent 2 px frames with no gap would read as one 4
            // px rule.
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UScrollBoxSlot_Padding);
            pad[0] = 0.f; pad[1] = 0.f; pad[2] = 0.f; pad[3] = kRowGapPx;
        }
    }
    // What the pointer was on before this pass rewrites the ids (the invalidation check after the
    // loop).
    const std::string hoveredIdBefore =
        (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_rowIds.size()))
            ? g_rowIds[static_cast<size_t>(g_hoverRow)] : std::string();
    const int total = U::ChildCount(g_list);
    // After the grow loop, clamped to what exists: BuildRow can fail mid-grow, and the hover walk
    // and the selection repaint both iterate this as "children actually shown".
    g_visibleRows = (total >= 0 && total < want) ? total : want;
    g_rowIds.assign(total < 0 ? 0 : total, std::string());

    const std::string own = sm::OwnLobbyId();
    for (int i = 0; i < total; ++i) {
        // The surplus branch first: a row past `want` needs only its box, to collapse it, and
        // deriving all seven parts cost ten dispatches per surplus row per sync; rows are never
        // removed, so on a list that was ever long the surplus is most of the loop.
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

        // Hidden (2), not Collapsed (1): collapsed takes no space, and an open lobby's name slid
        // left, the shift the always-present cell exists to prevent. One dispatch per row: the
        // glyph's bars are tinted once at build and only its visibility moves, so the lock neither
        // yellows on hover nor fades on a stale row (four more dispatches per painted row is not
        // worth it). ESlateVisibility: Visible=0, Collapsed=1, Hidden=2.
        if (rp.lock) E::SetWidgetVisibility(rp.lock, r.locked ? 0 : 2);
        SetRowText(rp.text[kCellName], isOwn ? r.name + "   (your server)" : r.name);
        SetRowText(rp.text[kCellFlag], MismatchMark(r));
        SetRowText(rp.text[kCellPlayers],
                   "Players: " + std::to_string(r.playersCur) + "/" +
                   std::to_string(r.playersMax));
        // Selection is keyed on the lobby, not the row index: the set churns and one host leaving
        // shifts every row after it, so an index-keyed selection would move to whatever landed
        // there. Read from r.lobbyId, not RowIsSelected(i): the assign above cleared g_rowIds and
        // this loop is refilling it.
        const bool sel = !g_selectedId.empty() && r.lobbyId == g_selectedId;
        ApplyRowSkin(rp, i == g_hoverRow, sel);
        ApplyRowTextColors(rp, r, isOwn, IsStale(r), i == g_hoverRow, sel);
        // The id is captured here, in the same pass as the text, so a click resolves to the server
        // the player was looking at.
        g_rowIds[static_cast<size_t>(i)] = r.lobbyId;
    }

    // The paint invalidates the hover: the tracker re-evaluates on motion, scroll or a count
    // change, and a re-sync that keeps the count while the membership shifts is none of those, so
    // the click at the next tick would read g_rowIds[g_hoverRow] naming a different server.
    // Dropping the index is right: the next real motion answers, and until then a click selects
    // nothing rather than the wrong thing.
    if (g_hoverRow >= 0) {
        const bool gone = g_hoverRow >= g_visibleRows ||
                          g_hoverRow >= static_cast<int>(g_rowIds.size()) ||
                          g_rowIds[static_cast<size_t>(g_hoverRow)] != hoveredIdBefore;
        if (gone) {
            g_hoverRow = -1;
            g_hover.Reset();   // or the tracker reports 'unchanged' and never re-asks
        }
    }

    // Every id this pass rendered is in g_rowIds, so the order check runs against what is on
    // screen.
    CheckOrderStable(want);

    // Put the view back where the player left it; the log line reports the one case that proves the
    // restore was needed.
    if (haveOffset && keepOffset > 0.f) {
        // Clamped: GetScrollOffset echoes the last request with no clamp of its own (measured), so
        // writing back an offset the shrunken list cannot honour stores a number the next
        // structural change reads as a real position. The bound is computed from the rows' own
        // metrics, not read back: GetScrollOffsetOfEnd reports last frame's extent, and this runs
        // in the tick that changes it.
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
            // The new list fits in the viewport: no position to hold, and 0 is "back to the top".
            U::SetScrollOffset(g_list, 0.f);
            UE_LOGI("server_browser_rows: rows %d -> %d while scrolled to %.1f -- the "
                    "shorter list no longer overflows, so the view returns to the top",
                    hadRows, want, keepOffset);
        }
    }
}

bool ClickSelect() {
    // A click on a hovered row selects it; the row under the cursor is already known from the hover
    // pass.
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
    // By id, never by index: g_rows is refreshed on a timer and its membership changes, so the
    // position that was selected need not hold the same server now.
    for (const Row& r : g_rows) {
        if (r.lobbyId == g_selectedId) { out = r; return true; }
    }
    // Selected, but the lobby is gone from the latest list (the host quit while the screen was
    // open): false, and the selection dropped so the highlight stops pointing at nothing.
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
