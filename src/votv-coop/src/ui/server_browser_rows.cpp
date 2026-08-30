// ui/server_browser_rows.cpp -- see ui/server_browser_rows.h.

#include "ui/server_browser_rows.h"

#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
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
// BOUNDS THE WHOLE SYNC LOOP, not just the display: `want` clamps to this, the grow
// loop is bounded by `want`, and `total = ChildCount` therefore never exceeds it. So
// this is a COMPUTE ceiling that happens to look like a display one.
// The comment that stood here -- "the master caps its list well below this" -- was
// FALSE (2026-08-26): `build_rows` (master.rs:531-553) emits every listed lobby,
// unbounded, so 100 servers render 64 and nothing logs the truncation.
// Raising it is step T2b and it CANNOT come first: see docs/MULTIPLAYER_UI.md
// section 8c.-1, which measures why the uncached walk per row must go first --
// raising the cap before that converts a latent defect into a 110-320 ms stall.
constexpr int   kMaxRows  = 64;

const FLinearColor kRowBg  = NS::RowBg();   // a list row at rest
const FLinearColor kRowSel = NS::RowSel();  // ...and selected. Fill, not text.
const FLinearColor kText   = NS::Text();    // the default: most text is white
const FLinearColor kAccent = NS::Accent();  // orange -- the interactive accent
const FLinearColor kHover  = NS::Hover();   // hover is a TEXT colour, see section 4
const FLinearColor kAmber  = NS::Amber();   // value emphasis; the mismatch tint
const FLinearColor kDim    = NS::Dim();     // secondary text (measured, not guessed)
const FLinearColor kOwn    = NS::Own();     // "your server" row

// ---- state (GAME THREAD ONLY) -----------------------------------------------------
void* g_list = nullptr;   // the UScrollBox holding the rows

// Style doc section 4: hover and selection are DIFFERENT channels. Hover recolours the
// row's TEXT; selection recolours the row's FILL. Both are applied on a CHANGE, never per
// frame.
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

// Build one row: USizeBox(64) -> UOverlay -> [ UImage background (the HIT TARGET),
// UHorizontalBox of five UTextBlocks ]. NO UButton: the native row's own `button_select`
// draws nothing in all three states, and a UButton would add a press visual we would then
// suppress.
void* BuildRow(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    if (!box) return nullptr;
    U::SetSizeBoxHeight(box, kRowH);
    void* ovl = Spawn(L"Overlay", box);
    void* bg  = ovl ? Spawn(L"Image", ovl) : nullptr;
    void* hb  = ovl ? Spawn(L"HorizontalBox", ovl) : nullptr;
    if (!ovl || !bg || !hb) return nullptr;
    // The background carries the row's state tint AND is the hit target, so it must be
    // Visible -- a HitTestInvisible image is not arranged the same way, and the geometry
    // walk reads the rect Slate cached for it.
    U::SetImageTintRaw(bg, kRowBg);
    E::SetWidgetVisibility(bg, 0);  // ESlateVisibility::Visible
    if (void* s = U::AddChild(ovl, bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
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
struct RowParts { void* box; void* bg; void* text[5]; };
bool RowPartsAt(int32_t i, RowParts& out) {
    out = RowParts{};
    void* box = U::ChildAt(g_list, i);
    if (!box) return false;
    out.box = box;
    void* ovl = nullptr;
    // LATCHED. `R::FindFunction` has no result cache and walks the whole GUObjectArray, and
    // this runs PER ROW inside Sync and RepaintRowFills -- 64 rows meant 64 full walks
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
    out.bg = U::ChildAt(ovl, 0);
    void* hb = U::ChildAt(ovl, 1);
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
    return std::wstring(cell.begin(), cell.end());
}

// A row's five text colours, from its data and its hover state. ONE owner: the sync pass
// and the hover edge both come through here, so "what colour is the version cell" is
// answered in exactly one place and un-hovering cannot restore the wrong base.
void ApplyRowTextColors(const RowParts& rp, const Row& r, bool isOwn, bool mismatch,
                        bool hovered) {
    // Style doc section 4: hovering turns the LABEL yellow and leaves the fill alone.
    // That is the opposite of the ImGui incumbent, which paints behind the row.
    const FLinearColor name = hovered ? kHover : (isOwn ? kOwn : kText);
    const FLinearColor body = hovered ? kHover : kText;
    const FLinearColor dim  = hovered ? kHover : kDim;
    const FLinearColor ver  = hovered ? kHover : (mismatch ? kAmber : kDim);
    if (rp.text[0]) E::SetTextBlockColor(rp.text[0], name);
    if (rp.text[1]) E::SetTextBlockColor(rp.text[1], body);
    if (rp.text[2]) E::SetTextBlockColor(rp.text[2], ver);
    if (rp.text[3]) E::SetTextBlockColor(rp.text[3], dim);
    if (rp.text[4]) E::SetTextBlockColor(rp.text[4], dim);
    (void)r;
}

void SetRowText(void* block, const std::string& utf8, const FLinearColor& col) {
    if (!block) return;
    const std::wstring w(utf8.begin(), utf8.end());
    E::SetWidgetText(block, w.c_str());
    E::SetTextBlockColor(block, col);
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
struct OrderDigest { uint64_t sequence = 0; uint64_t set = 0; };

OrderDigest DigestIds(const std::vector<std::string>& ids, int count) {
    OrderDigest d;
    d.sequence = 1469598103934665603ull;   // FNV-1a offset basis
    for (int i = 0; i < count && i < static_cast<int>(ids.size()); ++i) {
        uint64_t one = 1469598103934665603ull;
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

// Repaint every row's FILL from the current selection. Cheap (one tint per row, no
// reflection walk) and only ever called on a selection change.
void RepaintRowFills() {
    const int total = g_visibleRows;   // shown, not ChildCount's high-water mark
    for (int i = 0; i < total; ++i) {
        RowParts rp;
        if (!RowPartsAt(i, rp) || !rp.bg) continue;
        const bool sel = !g_selectedId.empty() &&
                         i < static_cast<int>(g_rowIds.size()) &&
                         g_rowIds[static_cast<size_t>(i)] == g_selectedId;
        U::SetImageTint(rp.bg, sel ? kRowSel : kRowBg);
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
    // Edge-applied: only the two rows that changed are recoloured.
    for (int i : {prev, hit}) {
        if (i < 0 || i >= static_cast<int>(g_rows.size())) continue;
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        const Row& r = g_rows[static_cast<size_t>(i)];
        const std::string own = sm::OwnLobbyId();
        bool mismatch = false;
        (void)VersionCell(r, mismatch);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, mismatch, i == hit);
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
    // `GetScrollOffset` reports Slate's DesiredScrollOffset -- what was last ASKED FOR,
    // measured 2026-08-26 to echo an unclamped request. That is the right value to carry
    // across: a wheel scroll goes through the same field, and a restore past the new end
    // is clamped by the next layout rather than by us guessing the new maximum.
    const int  hadRows    = g_visibleRows;   // read BEFORE the assignment below overwrites it
    const bool structural = (want != hadRows);
    float keepOffset = 0.f;
    const bool haveOffset = structural && hadRows > 0 && U::ScrollOffset(g_list, keepOffset);
    // The hover walk reads this instead of ChildCount: rows are GROWN and never
    // removed (a surplus row is Collapsed below, not destroyed), so ChildCount is a
    // HIGH-WATER MARK -- once the list has held 64 it reports 64 forever, and the walk
    // would read the geometry of rows that are not on screen. A collapsed widget is not
    // arranged, so it keeps its last painted rect, which can still contain the cursor
    // and win the hover shortcut over the real row beneath it.
    g_visibleRows = want;
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
    const int total = U::ChildCount(g_list);
    g_rowIds.assign(total < 0 ? 0 : total, std::string());

    const std::string own = sm::OwnLobbyId();
    for (int i = 0; i < total; ++i) {
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        if (i >= want) {
            E::SetWidgetVisibility(rp.box, 1);  // ESlateVisibility::Collapsed
            continue;
        }
        E::SetWidgetVisibility(rp.box, 0);
        const Row& r = g_rows[static_cast<size_t>(i)];
        const bool isOwn = !own.empty() && r.lobbyId == own;
        bool mismatch = false;
        const std::wstring ver = VersionCell(r, mismatch);

        SetRowText(rp.text[0], isOwn ? r.name + "   (your server)" : r.name, kText);
        SetRowText(rp.text[1], std::to_string(r.playersCur) + "/" + std::to_string(r.playersMax),
                   kText);
        if (rp.text[2]) E::SetWidgetText(rp.text[2], ver.c_str());
        SetRowText(rp.text[3], r.world, kDim);
        SetRowText(rp.text[4], std::to_string(r.ageSec) + "s", kDim);
        ApplyRowTextColors(rp, r, isOwn, mismatch, i == g_hoverRow);
        // SELECTION IS KEYED ON THE LOBBY, NOT THE ROW INDEX. The master returns lobbies
        // from a HashMap and nothing sorts them (MULTIPLAYER_UI section 8c.-1), so a
        // refresh can put a different server at index N -- an index-keyed selection would
        // silently move to whatever landed there, which is how a player joins a server
        // they did not pick.
        if (rp.bg)
            U::SetImageTint(rp.bg, (!g_selectedId.empty() && r.lobbyId == g_selectedId)
                                       ? kRowSel : kRowBg);
        // The id is captured HERE, in the same pass as the text above it, so a click
        // resolves to the server the user was LOOKING at even if the master reorders.
        g_rowIds[static_cast<size_t>(i)] = r.lobbyId;
    }

    // Every id this pass rendered is now in g_rowIds, so the order invariant can be checked
    // against what is actually ON SCREEN rather than against the network list.
    CheckOrderStable(want);

    // Put the view back where the player left it. The log line is the MEASUREMENT this
    // ships with: nothing has ever observed whether a grow/collapse actually moves this
    // box, so the restore reports the one case that proves it was needed -- a structural
    // change while the list was scrolled away from the top.
    if (haveOffset && keepOffset > 0.f) {
        U::SetScrollOffset(g_list, keepOffset);
        UE_LOGI("server_browser_rows: rows %d -> %d while scrolled to %.1f -- offset "
                "restored", hadRows, want, keepOffset);
    }
}

bool ClickSelect() {
    // A click on a hovered row SELECTS it. The row under the cursor is already known from
    // the hover pass, so this costs no extra dispatch.
    if (g_hoverRow < 0 || g_hoverRow >= static_cast<int>(g_rowIds.size())) return false;
    const std::string& id = g_rowIds[static_cast<size_t>(g_hoverRow)];
    if (id.empty() || id == g_selectedId) return false;
    g_selectedId = id;
    RepaintRowFills();
    UE_LOGI("server_browser_rows: row selected (%s)", id.c_str());
    return true;
}

int HoveredRow() { return g_hoverRow; }

const char* SelectedId() { return g_selectedId.c_str(); }

bool Selected(coop::net::lobby::LobbyRow& out) {
    if (g_selectedId.empty()) return false;
    // BY ID, never by index -- this module's header invariant. `g_rows` is refreshed from
    // the master on a timer and is not sorted, so the position that was selected is not the
    // position that holds it now, and a positional read would hand a player a different
    // server than the one they clicked.
    for (const Row& r : g_rows) {
        if (r.lobbyId == g_selectedId) { out = r; return true; }
    }
    // Selected, but the lobby is gone from the latest list: the host quit while the screen
    // was open. Answering false is right -- the caller has nothing to connect to -- and the
    // selection is dropped so the highlight stops pointing at a server that is not there.
    g_selectedId.clear();
    RepaintRowFills();
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
    dump("row.bg", rp.bg);
    dump("row.text0", rp.text[0]);
}

}  // namespace ui::server_browser_rows
