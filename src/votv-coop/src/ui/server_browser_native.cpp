// ui/server_browser_native.cpp -- see ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
#include "ui/boot_warning_dialog.h"     // the loud failure surface for a donor that never appears
#include "ui/input_focus.h"            // a click only counts while OUR window is foreground
#include "ui/native_screen.h"          // palette + widget primitives, shared with the host window
#include "ui/server_browser_actions.h"   // CONNECT / HOST / REFRESH, its own TU
#include "ui/server_browser_selftest.h"  // the dev phase machine; ships dark
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace ui::server_browser_native {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace sm = coop::session_manager;
namespace selftest = ui::server_browser_selftest;

using ue_wrap::FLinearColor;
using Row = coop::net::lobby::LobbyRow;

// ---- layout constants ------------------------------------------------------------
// Slate units. The row height is the game's own (uicomp_saveSlot_C is 64 px, section 7b),
// so a row of ours is the same size as a row of theirs.
constexpr float kWindowW  = 980.f;
constexpr float kWindowH  = 620.f;
constexpr float kRowH     = 64.f;
// Frame + spacing, from the native windows (style doc section 3).
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
constexpr float kRowGapPx = 2.f;
// The list's height is EXPLICIT, not the VerticalBox's leftover slack.
//
// MEASURED 2026-08-26: with a Fill slot the box allotted the ScrollBox 542 px inside a
// window that only had ~484 px left for it (offsetOfEnd 1438 against 30 rows of 66 px puts
// the viewport at 542), so the list overflowed UPWARD and its first row was drawn clipped
// under the column header. Slack arithmetic depends on every sibling's desired size being
// what you assumed; an override depends on nothing.
constexpr float kListH    = 470.f;
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
// ---- the construction kit ----------------------------------------------------------
// Alignment enums, the palette, and the widget primitives moved to ui/native_screen
// (2026-08-29) when the hosting window became the second native screen. The names are
// re-bound here so every call site below reads exactly as it did -- the extraction is a
// MOVE, and a move that rewrites its call sites cannot be diffed against the original.
namespace NS = ui::native_screen;
using NS::kFill;
using NS::kLeft;
using NS::kCenter;
using NS::kJustLeft;
using NS::kJustCenter;
using NS::ReadPtr;
using NS::SwitcherChild;
using NS::DonorField;
using NS::Spawn;
using NS::AddText;
using NS::AddFramedBox;
using NS::BuildButton;

const FLinearColor kPanel  = NS::Panel();   // window fill
const FLinearColor kBorder = NS::Border();  // every frame in the game's menus
const FLinearColor kRowBg  = NS::RowBg();   // a list row at rest
const FLinearColor kRowSel = NS::RowSel();  // ...and selected. Fill, not text.
const FLinearColor kText   = NS::Text();    // the default: most text is white
const FLinearColor kAccent = NS::Accent();  // orange -- the interactive accent
const FLinearColor kHover  = NS::Hover();   // hover is a TEXT colour, see section 4
const FLinearColor kAmber  = NS::Amber();   // value emphasis; the mismatch tint
const FLinearColor kDim    = NS::Dim();     // secondary text (measured, not guessed)
const FLinearColor kOwn    = NS::Own();     // "your server" row

// ---- state (GAME THREAD ONLY unless marked) --------------------------------------
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_list     = nullptr;   // the UScrollBox holding the rows
void* g_status   = nullptr;   // the footer UTextBlock
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
void* g_closeBtn = nullptr;   // the X, top-right of the title row
void* g_backBtn  = nullptr;   // BACK, bottom-right beside the status line
// LBUTTON edge state for the chrome poll. Primed on Show() so the very release that
// OPENED the screen cannot be read as a click on the X sitting under the cursor.
bool  g_prevLmb   = false;
bool  g_lmbPrimed = false;

// ---- row state (style doc section 4: hover and selection are DIFFERENT channels) ----
// Hover recolours the row's TEXT; selection recolours the row's FILL. Both are applied on
// a CHANGE, never per frame.
int         g_hoverRow   = -1;   // index into the live rows, or -1
NS::HoverTracker g_hover;        // pointer + scroll + settling, one owner
std::string g_selectedId;        // the SELECTED LOBBY, keyed by id and not by index
// ESC edge state. Primed on the first tick a screen is shown so a key already held when it
// opens cannot synthesize a close -- the same guard multiplayer_menu's click poll uses.
bool  g_prevEsc   = false;
bool  g_escPrimed = false;
int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;

// Row identity. See the header, invariant 1: `g_rowIds[i]` is the lobbyId the child at
// index i was RENDERED with, written in the same pass as that child's text by the single
// function that owns the panel. Positional INSIDE this one structure is fine; positional
// across the network list and the displayed list is the defect this exists to avoid.
std::vector<std::string> g_rowIds;
std::vector<Row> g_rows;
uint64_t g_lastRefreshMs = 0;
uint64_t g_lastRowsGen   = 0;   // the fetch generation last PAINTED (see OnMenuTick)
// A click's answer holds the footer for this long against the list sync's rewrite.
constexpr uint64_t kNoticeMs = 6000;
uint64_t g_noticeUntilMs = 0;
int      g_visibleRows   = 0;   // rows actually SHOWN, not ChildCount's high-water mark
int      g_buildAttempts = 0;
bool     g_toldTheUser   = false;

// Cross-thread: the deferred open intent and the pointer-moved flag.
std::atomic<uint64_t> g_wantOpenMs{0};   // 0 = no intent
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs  = 20000;  // a join that never returns to a menu must expire
// FIVE SECONDS, WHICH IS WHAT WAS DECIDED. This read 1000 while
// docs/MULTIPLAYER_UI.md section 8c.-1 recorded "USER 2026-08-26: the re-fetch cadence is
// 5 s" -- and that section's own arithmetic is computed against 5 s ("one stalled frame
// per sync is 0.17% of frames at the 5 s cadence"), so at 1 Hz every cost number in the
// perf lane was understated 5x. It also gates `sm::Refresh()`, which spawns a detached
// thread and a TLS handshake per fetch, so the old value paid five of those per five
// seconds for a list that changes on the scale of a person deciding to host.
constexpr uint64_t kRefreshMs    = 5000;

// The five columns, in the order section 7c fixes them, with their fill weights.
struct Column { const wchar_t* title; float weight; };
constexpr Column kColumns[5] = {
    {L"Name",    0.44f},
    {L"Players", 0.12f},
    {L"Version", 0.20f},
    {L"World",   0.14f},
    {L"Age",     0.10f},
};

// ---- the screen -------------------------------------------------------------------

// Build one row: USizeBox(64) -> UOverlay -> [ UImage background (the HIT TARGET),
// UHorizontalBox of five UTextBlocks ]. NO UButton: the native row's own `button_select`
// draws nothing in all three states, `IsHovered()` is owned by UWidget so a bare UImage
// answers it (measured), and a UButton would add a press visual we would then suppress.
void* BuildRow(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    if (!box) return nullptr;
    U::SetSizeBoxHeight(box, kRowH);
    void* ovl = Spawn(L"Overlay", box);
    void* bg  = ovl ? Spawn(L"Image", ovl) : nullptr;
    void* hb  = ovl ? Spawn(L"HorizontalBox", ovl) : nullptr;
    if (!ovl || !bg || !hb) return nullptr;
    // The background carries the row's state tint AND is the hit target, so it must be
    // Visible -- a SelfHitTestInvisible image answers IsHovered() == false, which would
    // look exactly like the whole approach failing.
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
    // this runs PER ROW inside SyncRows and RepaintRowFills -- 64 rows meant 64 full walks
    // landing in one frame, every sync. `R::FindClass` beside it has been cached since
    // `ca1cd5e4`; only this one was not. A UFunction never moves, so one resolve is all
    // there is. (Post-ship perf audit, 2026-08-29. The general fix -- a cache inside
    // `FindFunction` for all 476 call sites -- is filed separately and is not this lane.)
    // ONCE, INCLUDING ON FAILURE. The first version latched only success, so a miss re-walked
    // the whole GUObjectArray on every call -- and this runs per ROW inside SyncRows and
    // RepaintRowFills, so the failure path was 64 full walks per frame. A UFunction, unlike
    // an asset, either exists at process start or never does, so a permanent negative latch
    // is not merely safe here, it is correct.
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
//
// `NotePointerMoved()` used to exist for this and was never wired to anything -- no
// caller, no consumer. GetCursorPos here is one syscall, needs no edit to the overlay's
// input path, and sees movement regardless of how the message was routed; the dead seam is
// retired with it (RULE 2).
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
// which is what makes the positional pairing safe (header, invariant 1).
void SyncRows() {
    g_lastRowsGen = sm::CopyRows(g_rows);
    const int want = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows
                                                               : static_cast<int>(g_rows.size());
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
    // The footer mirrors the session's own status EXCEPT while a click's answer is still
    // fresh. Without this the list sync would wipe "Pick a server from the list first."
    // within a few frames of the player reading it, which reads as the button doing nothing.
    if (g_status && ::GetTickCount64() >= g_noticeUntilMs) {
        const std::string s = sm::Status();
        const std::wstring w(s.begin(), s.end());
        E::SetWidgetText(g_status, w.c_str());
    }
}

// Build the screen once per menu instance. FAIL-CLOSED: a null donor means DO NOT BUILD
// and retry, never fall back to a default style -- that fallback is the Roboto/centred/
// white bug. After enough attempts the user is TOLD, because a silent forever-retry is the
// same defect one level quieter.
bool BuildScreen(void* switcher) {
    void* saveSlots = SwitcherChild(switcher, L"ui_saveSlots_C");
    void* settings  = SwitcherChild(switcher, L"ui_settings_C");
    void* fillDonor = DonorField(saveSlots, L"Image_0");
    void* barDonor  = DonorField(settings,  L"scrollboxRoot");
    // button_back joins the REQUIRED set rather than degrading to an unstyled button,
    // because the chrome it styles is the way OUT of this screen. An unstyled X that
    // still closes would be tolerable; a missing donor means we do not know what else
    // moved in this build, and the fail-closed rule exists for exactly that.
    void* backDonor = DonorField(saveSlots, L"button_back");
    if (!fillDonor || !barDonor || !backDonor) {
        if (++g_buildAttempts >= 15 && !g_toldTheUser) {
            g_toldTheUser = true;
            UE_LOGE("server_browser_native: donors still absent after %d attempts "
                    "(ui_saveSlots_C.Image_0=%p ui_settings_C.scrollboxRoot=%p "
                    "ui_saveSlots_C.button_back=%p) -- NOT building",
                    g_buildAttempts, fillDonor, barDonor, backDonor);
            ui::boot_warning_dialog::Arm(
                "Multivoid: the multiplayer screen could not be built.\n"
                "A required menu element was not found in this game build "
                "(ui_saveSlots.Image_0 / ui_settings.scrollboxRoot / "
                "ui_saveSlots.button_back).\n"
                "This usually means the game updated and the mod needs a new release.");
        }
        return false;
    }

    void* root = Spawn(P::name::UserWidgetClass, switcher);
    void* tree = root ? Spawn(P::name::WidgetTreeClass, root) : nullptr;
    void* ovl  = tree ? Spawn(L"Overlay", tree) : nullptr;
    if (!root || !tree || !ovl) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = ovl;

    // (1) The SCRIM. Copied from the game: ui_saveSlots_C's own first child Image_302 is a
    // full-screen UImage with TintColor (0,0,0,0.5) and NO ResourceObject, and that is what
    // dims the menu behind every native sub-screen. It needs no donor, and being Visible is
    // what makes it absorb a click that misses the window.
    void* scrim = Spawn(L"Image", ovl);
    if (!scrim) return false;
    g_scrimW = scrim;
    U::SetImageTintRaw(scrim, FLinearColor{0.f, 0.f, 0.f, 0.5f});
    E::SetWidgetVisibility(scrim, 0);
    if (void* s = U::AddChild(ovl, scrim))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);

    // (2) The WINDOW: a centred SizeBox holding a fill image and the content column.
    void* winBox = Spawn(L"SizeBox", ovl);
    // THE WINDOW IS A FRAMED BOX, not a cloned 9-slice. Until 2026-08-26 this cloned
    // ui_saveSlots.Image_0's brush, which gave a soft borderless panel -- and every window
    // in VOTV's own menus is a 2 px #646464 frame around a #1A1A1A fill with sharp corners
    // (docs/VOTV_UI_STYLE.md section 3). The clone was the closest thing available before
    // anyone had measured the real treatment; now it is measured, so we author it.
    // `fillDonor` stays REQUIRED -- see the donor guard above; it is the canary for "did
    // this menu's class layout move", and dropping the check would trade a loud failure
    // for a silent one.
    void* winOvl = winBox ? AddFramedBox(winBox, kPanel, kBorderPx) : nullptr;
    void* col    = winOvl ? Spawn(L"VerticalBox", winOvl) : nullptr;
    if (!winBox || !winOvl || !col) return false;
    U::SetSizeBoxWidth(winBox, kWindowW);
    U::SetSizeBoxHeight(winBox, kWindowH);
    if (void* s = U::AddChild(winOvl, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                             P::off::UOverlaySlot_Padding);
        pad[0] = pad[1] = pad[2] = pad[3] = kPadPx;
    }
    U::SetContent(winBox, winOvl);
    if (void* s = U::AddChild(ovl, winBox))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        kCenter, kCenter);

    // (3) The content column: title, column headers, the list, the status line.
    // The title row is a BOX, not a lone text block, so the X has somewhere to sit. Until
    // 2026-08-26 this screen had no chrome at all and ESC was the only way out -- the
    // user's words were "I don't even see the X to close server browser window".
    // THE TITLE STRIP. Native windows put a centred white title on its own bordered strip
    // (style doc section 3), so the title gets a frame of its own rather than floating.
    // The X rides in the same strip at the right.
    if (void* titleBox = AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* titleRow = Spawn(L"HorizontalBox", titleBox)) {
            // USER 2026-08-26: "The windows title should say something like Multivoid -
            // Server Browser and be in the style of votv, not the current colors." That
            // settles style doc section 6's open product call in the direction of the
            // game: native titles are WHITE, centred and larger, and the coop cyan appears
            // in no VOTV menu. The build identity the title used to carry is simply GONE
            // from this screen: the main menu shows "Multivoid <game> b<build>" in its top
            // left at all times, so a second copy inside the browser was redundant -- the
            // user said so after seeing it parked in the footer, which is where this first
            // moved it. The per-row Version column still carries what a player actually
            // needs here, which is each SERVER's pair, not ours.
            AddText(titleRow, L"Multivoid  -  Server Browser", 24, kText,
                              kJustCenter, 1.f);
            g_closeBtn = BuildButton(titleRow, backDonor, L"X", 20);
            if (void* s = U::AddChild(titleBox, titleRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                kFill, kCenter);
        }
        if (void* s = U::AddChild(col, titleBox)) {
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UVerticalBoxSlot_Padding);
            pad[0] = pad[1] = pad[2] = 0.f; pad[3] = kPadPx;
        }
    }
    // Column headers are a SECTION HEADER, and those are orange in every native window.
    void* head = Spawn(L"HorizontalBox", col);
    if (head) {
        for (const Column& c : kColumns) AddText(head, c.title, 16, kAccent, kJustLeft, c.weight);
        U::AddChild(col, head);
    }
    void* listBox = Spawn(L"SizeBox", col);
    g_list = listBox ? Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !g_list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    // The settings list's scrollbar treatment (section 7b): a server list is the long-list
    // case, and ui_saveSlots' own ScrollBox sets no bar style at all. NINE brushes.
    U::CloneStyle(g_list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    U::SetContent(listBox, g_list);
    if (void* s = U::AddChild(col, listBox))
        U::SetSlotAlign(s, P::off::UVerticalBoxSlot_HAlign, P::off::UVerticalBoxSlot_VAlign,
                        kFill, kFill);
    // The footer mirrors the title row: the status line takes the slack, BACK sits at the
    // right. Two exits, because the game's own sub-screens carry a button_back and a
    // player who does not think to press ESC should not be stranded.
    // THE FOOTER STRIP, and BACK sits at its LEFT.
    //
    // That placement is not a preference. Every native window that has both puts Back
    // bottom-LEFT and its actions bottom-RIGHT -- Settings is `Back | Reset all Apply`,
    // Keybinds is `Back | Hard reset`, the save browser is `Back` alone at the left. Our
    // first version put BACK bottom-right, which is the CONFIRM position, so a cancel
    // control sat where the game trains players to expect a commit (style doc section 5,
    // gap S7). Connect/Join will land at the right when T7 adds it, and then the row reads
    // the way every other VOTV window does.
    if (void* footBox = AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* footRow = Spawn(L"HorizontalBox", footBox)) {
            g_backBtn = BuildButton(footRow, backDonor, L"BACK", 18);
            g_status  = AddText(footRow, L"", 16, kText, kJustLeft, 1.f);
            // The status has fill weight 1, so it takes all the slack and pushes these to
            // the right edge -- Back at the left, actions at the right, which is what every
            // native VOTV window does (style doc section 5, gap S7).
            if (!ui::server_browser_actions::Build(footRow, backDonor)) return false;
            if (void* s = U::AddChild(footBox, footRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                kFill, kCenter);
        }
        if (void* s = U::AddChild(col, footBox)) {
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UVerticalBoxSlot_Padding);
            pad[0] = pad[1] = pad[3] = 0.f; pad[2] = kPadPx;
        }
    }

    g_root  = root;
    g_rowIds.clear();
    UE_LOGI("server_browser_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, g_list, g_buildAttempts + 1);
    return true;
}

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    g_priorIndex = U::SwitcherIndex(g_switcher);
    if (g_ourIndex < 0) {
        U::AddChild(g_switcher, g_root);
        g_ourIndex = U::ChildCount(g_switcher) - 1;
    }
    // The screen stays ATTACHED for the menu's life -- a switcher renders only its active
    // child, so an inactive 12th child costs nothing, and rebuilding N rows on every open
    // would churn GC for no reason. Only the index moves.
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    g_escPrimed = false;   // re-prime: an ESC held while the screen opens must not close it
    g_lmbPrimed = false;   // ...and the same for the release that OPENED us
    SyncRows();
    UE_LOGI("server_browser_native: shown (index %d -> %d)", g_priorIndex, g_ourIndex);
}

void Hide(const char* why) {
    if (!g_shown) return;
    g_shown = false;
    // Restore ONLY if the index is still ours: the game's own sibling screens write this
    // field to navigate, and stomping a navigation the player just made would be worse than
    // leaving it.
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == g_ourIndex && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("server_browser_native: hidden (%s; index was %d, ours %d)", why, now, g_ourIndex);
}

bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::browser_native);
    return s;
}
bool AutoOpenArmed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::browser_autoopen);
    return s && Armed();
}

}  // namespace

void Open() {
    g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

void Close() {
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    // The hide touches the engine, so it must run on the GAME THREAD; this only records
    // the request and OnMenuTick performs it. Close() is called from the ImGui picker and
    // from the harness, neither of which is the game thread by construction.
    g_wantClose.store(true, std::memory_order_relaxed);
}

void CloseNow() {
    // ENFORCED, not merely documented. It sits one declaration below `Close()`, whose header
    // says "safe from any thread", and it reaches ProcessEvent through SwitcherSetIndex --
    // so the difference between them is exactly the kind a caller reads past. Off-thread it
    // degrades to the deferred close rather than touching the engine.
    if (!ue_wrap::game_thread::IsGameThread()) {
        UE_LOGW("server_browser_native: CloseNow off the game thread -- deferring instead "
                "(it drives the switcher through ProcessEvent)");
        Close();
        return;
    }
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    g_wantClose.store(false, std::memory_order_relaxed);   // performed here, not deferred
    Hide("replaced by a sibling screen");
}

bool IsOpen() { return g_shown; }

int HoveredRow() { return g_hoverRow; }
const char* SelectedRowId() { return g_selectedId.c_str(); }

bool SelectedRow(coop::net::lobby::LobbyRow& out) {
    if (g_selectedId.empty()) return false;
    // BY ID, never by index -- invariant 1 in this module's header. `g_rows` is refreshed
    // from the master on a timer and is not sorted, so the position that was selected is not
    // the position that holds it now, and a positional read would hand a player a different
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

void SetNotice(const char* text) {
    if (!text) return;
    g_noticeUntilMs = ::GetTickCount64() + kNoticeMs;
    if (!g_status) return;
    const std::string s(text);
    const std::wstring w(s.begin(), s.end());
    E::SetWidgetText(g_status, w.c_str());
}

void LogRowHitDiagnostics(int32_t i) {
    RowParts rp;
    if (!RowPartsAt(i, rp)) {
        UE_LOGW("server_browser_native: row %d has no parts -- RowPartsAt failed, so it has "
                "no background to tint and no text to recolour", i);
        return;
    }
    auto dump = [](const char* what, void* w) {
        if (!w) { UE_LOGW("server_browser_native:   %s is NULL", what); return; }
        ue_wrap::FVector2D tl{}, sz{};
        const bool haveRect = U::WidgetScreenRect(w, tl, sz);
        UE_LOGW("server_browser_native:   %s hovered=%d rect %s(%.0f,%.0f) %.0fx%.0f",
                what, E::WidgetIsHovered(w) ? 1 : 0, haveRect ? "" : "UNREAD ",
                tl.X, tl.Y, sz.X, sz.Y);
        U::LogVisibilityChain(what, w);
    };
    dump("row.bg", rp.bg);
    dump("row.text0", rp.text[0]);
}


void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // Rebuild on a new menu instance (the old widgets died with it).
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_list = nullptr; g_status = nullptr;
        g_closeBtn = nullptr; g_backBtn = nullptr; g_scrimW = nullptr;
        ui::server_browser_actions::Forget();
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_toldTheUser = false;
        g_rowIds.clear();
    }
    if (!g_root) {
        if (!BuildScreen(switcher)) return;
        if (AutoOpenArmed()) {
            UE_LOGW("server_browser_native: [dev] browser_autoopen=1 -- showing without a click");
            Open();
            selftest::Arm();
        }
    }
    g_menu = menu;

    // Consume the deferred intent. This IS the world gate: we are inside a MAIN-menu tick,
    // which is a first-hand positive observation that the menu is up and ticking -- strictly
    // stronger than any memoised world reading, and it cannot fire over gameplay.
    if (g_wantClose.exchange(false, std::memory_order_relaxed)) Hide("requested");
    const uint64_t want = g_wantOpenMs.load(std::memory_order_relaxed);
    if (want) {
        const uint64_t age = ::GetTickCount64() - want;
        if (age > kIntentTtlMs) {
            g_wantOpenMs.store(0, std::memory_order_relaxed);
            UE_LOGW("server_browser_native: open intent EXPIRED after %llu ms without a main "
                    "menu -- dropped rather than left armed", static_cast<unsigned long long>(age));
        } else {
            g_wantOpenMs.store(0, std::memory_order_relaxed);
            Show();
        }
    }

    // THE SELF-CHECK TICKS WHETHER OR NOT THE SCREEN IS SHOWN, and that is deliberate:
    // its last phases RE-OPEN the screen after ESC has closed it, so they can then drive
    // the X. Below the `!g_shown` return it would stop ticking the moment its own ESC
    // phase succeeded, and the chrome would stay untested forever. Every phase that needs
    // a visible screen runs before that point, in order.
    selftest::Tick(g_scrimW, g_list, g_closeBtn);

    if (!g_shown) return;

    // RECONCILE, do not assert. A sibling screen (or ESC reaching a stale `widgetEnter`,
    // which the game clears only on its own ESC path) can write ActiveWidgetIndex away from
    // ours; if that happened we were closed, whoever did it.
    if (U::SwitcherIndex(g_switcher) != g_ourIndex) {
        g_shown = false;
        UE_LOGI("server_browser_native: the switcher moved off our index -- treating as closed");
        return;
    }

    // ESC CLOSES THE SCREEN, and until the chrome exists this is the ONLY way out.
    //
    // The game's own ESC cannot help us: `ui_menu_C::OnKeyDown` casts `widgetEnter` to
    // int_widgets and then tests `ActiveWidgetIndex == 0`, and at our index BOTH fail, so
    // it is a measured no-op (section 8). That is fine for ui_saveSlots, which has a
    // button_back -- it was NOT fine here, where Close() had no callers at all and the
    // screen stranded the player at the menu with nothing to press. Exactly the hazard
    // section 8 wrote down for the RUNG 1 probe, which got a deadline and an auto-restore;
    // this got neither until 2026-08-26.
    //
    // Polled here rather than in the WndProc detour: this observer already runs every menu
    // tick, the poll costs one GetAsyncKeyState, and it keeps the change inside this TU --
    // no edit to the overlay's input path or to the one hands-on-verified inject. We do
    // not swallow the key; the game's handler runs too and is a no-op at our index (or
    // navigates away on a stale widgetEnter, which the reconcile below then observes).
    {
        const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
        const bool pressEdge = esc && !g_prevEsc;
        // (A one-shot 'ESC produced no edge' diagnostic stood here until 2026-08-29. It
        // was written to find why ESC did not close the screen; the cause turned out to
        // be that no input reached the game at all while an ImGui surface held capture,
        // which this could never have shown. RULE 2: it goes with its question.)
        g_prevEsc = esc;
        if (pressEdge) {
            Hide("ESC");
            return;
        }
    }

    // THE CHROME CLICK, on the LBUTTON RELEASE edge -- the same shape as the shipped,
    // hands-on-verified MULTIPLAYER button (multiplayer_menu.cpp:253-271) and for the same
    // reason: our buttons are real UButtons, so the mouse-DOWN drives Slate's Pressed
    // visual and acting on the down edge would close the screen out from under a button
    // that never saw its own release. Releasing lets it complete press->spring-back first.
    //
    // IsHovered() is a UFunction and is called ONLY on the release edge, never per frame.
    {
        const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
        const bool releaseEdge = !down && g_prevLmb;
        g_prevLmb = down;
        if (releaseEdge && ui::input_focus::IsOurWindowForeground()) {
            if (g_closeBtn && E::WidgetIsHovered(g_closeBtn)) { Hide("X"); return; }
            if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
            // The action bar BEFORE the rows: its buttons sit in the footer, outside the
            // list, so they cannot both answer -- but returning here is what keeps a click
            // on CONNECT from also being read as a click on whatever is behind it.
            if (ui::server_browser_actions::OnReleaseEdge()) return;
            // A click on a hovered row SELECTS it. The row under the cursor is already
            // known from the hover pass, so this costs no extra dispatch.
            if (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_rowIds.size())) {
                const std::string& id = g_rowIds[static_cast<size_t>(g_hoverRow)];
                if (!id.empty() && id != g_selectedId) {
                    g_selectedId = id;
                    RepaintRowFills();
                    UE_LOGI("server_browser_native: row selected (%s)", id.c_str());
                }
            }
        }
    }

    UpdateHover();

    // FETCH ON A TIMER, PAINT ON AN ARRIVAL -- two questions, and they used to share one
    // gate. With paint coupled to the fetch tick, a lobby that arrived at t=0.3 s was not
    // drawn until t=5 s, and REFRESH called `sm::Refresh()` with no repaint at all, so the
    // button showed "Refreshing..." over an unchanged list and read as dead. `CopyRows`
    // already returns a generation that increments per completed fetch and `SyncRows` was
    // throwing it away; the sibling window has used exactly this shape (`g_savesRev`) since
    // it was written.
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastRefreshMs >= kRefreshMs) {
        g_lastRefreshMs = now;
        sm::Refresh();
    }
    if (sm::RowsGeneration() != g_lastRowsGen) SyncRows();
}

}  // namespace ui::server_browser_native
