// ui/server_browser_native.cpp -- see ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
#include "ui/boot_warning_dialog.h"     // the loud failure surface for a donor that never appears
#include "ui/server_browser_selftest.h"  // the dev phase machine; ships dark
#include "ue_wrap/core/call.h"
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
// A lone FSlateBrush copied on its own is still a brush: its unreflected resource handle
// sits at +0x70 of the copy, i.e. offset 0 in the table CloneStyle takes.
constexpr size_t kOneBrush[1] = {0};

// EHorizontalAlignment / EVerticalAlignment: Fill=0 Left=1/Top=1 Center=2 Right=3/Bottom=3.
constexpr uint8_t kFill = 0, kLeft = 1, kCenter = 2;
// ETextJustify: Left=0 Center=1 Right=2.
constexpr uint8_t kJustLeft = 0, kJustCenter = 1;

const FLinearColor kText  {0.86f, 0.90f, 0.94f, 1.f};
const FLinearColor kDim   {0.55f, 0.60f, 0.66f, 1.f};
const FLinearColor kAccent{0.00f, 1.00f, 1.00f, 1.f};  // the coop cyan, as on the menu button
const FLinearColor kAmber {1.00f, 0.78f, 0.35f, 1.f};  // version mismatch -- ported from the incumbent
const FLinearColor kOwn   {0.62f, 0.92f, 0.70f, 1.f};

// ---- state (GAME THREAD ONLY unless marked) --------------------------------------
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_list     = nullptr;   // the UScrollBox holding the rows
void* g_status   = nullptr;   // the footer UTextBlock
void* g_title    = nullptr;
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
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
int      g_buildAttempts = 0;
bool     g_toldTheUser   = false;

// Cross-thread: the deferred open intent and the pointer-moved flag.
std::atomic<uint64_t> g_wantOpenMs{0};   // 0 = no intent
std::atomic<bool>     g_wantClose{false};
std::atomic<bool>     g_pointerMoved{false};
constexpr uint64_t kIntentTtlMs  = 20000;  // a join that never returns to a menu must expire
constexpr uint64_t kRefreshMs    = 1000;

inline void* ReadPtr(void* base, int32_t off) {
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
    U::SetImageTintRaw(bg, FLinearColor{1.f, 1.f, 1.f, 0.05f});
    E::SetWidgetVisibility(bg, 0);  // ESlateVisibility::Visible
    if (void* s = U::AddChild(ovl, bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* s = U::AddChild(ovl, hb))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    for (const Column& c : kColumns)
        AddText(hb, L"", 16, kText, kJustLeft, c.weight);
    // SizeBox is a UContentWidget: its single child goes through SetContent.
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fn);
            f.Set<void*>(L"Content", ovl);
            Call(box, f);
        }
    }
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
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, L"GetContent")) {
            ue_wrap::ParamFrame f(fn);
            if (Call(box, f)) ovl = f.Get<void*>(L"ReturnValue");
        }
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

void SetRowText(void* block, const std::string& utf8, const FLinearColor& col) {
    if (!block) return;
    const std::wstring w(utf8.begin(), utf8.end());
    E::SetWidgetText(block, w.c_str());
    E::SetTextBlockColor(block, col);
}

// THE SINGLE WRITER of both the children's text and g_rowIds. Nothing else touches either,
// which is what makes the positional pairing safe (header, invariant 1).
void SyncRows() {
    sm::CopyRows(g_rows);
    const int want = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows
                                                               : static_cast<int>(g_rows.size());
    int have = U::ChildCount(g_list);
    if (have < 0) have = 0;
    // GROW by building; SHRINK by COLLAPSING, never by detaching. Nothing roots a detached
    // row -- the panel's Slots is the only reference -- so a detached free-list would be a
    // use-after-free waiting for the next GC. A collapsed row stays rooted, is not
    // hit-testable, and a regrow just un-collapses it.
    for (int i = have; i < want; ++i) {
        void* row = BuildRow(g_list);
        if (!row) break;
        if (void* s = U::AddChild(g_list, row))
            U::SetSlotAlign(s, P::off::UScrollBoxSlot_HAlign, P::off::UScrollBoxSlot_VAlign,
                            kFill, kCenter);
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

        SetRowText(rp.text[0], isOwn ? r.name + "   (your server)" : r.name, isOwn ? kOwn : kText);
        SetRowText(rp.text[1], std::to_string(r.playersCur) + "/" + std::to_string(r.playersMax),
                   kText);
        if (rp.text[2]) {
            E::SetWidgetText(rp.text[2], ver.c_str());
            E::SetTextBlockColor(rp.text[2], mismatch ? kAmber : kDim);
        }
        SetRowText(rp.text[3], r.world, kDim);
        SetRowText(rp.text[4], std::to_string(r.ageSec) + "s", kDim);
        // The id is captured HERE, in the same pass as the text above it, so a click
        // resolves to the server the user was LOOKING at even if the master reorders.
        g_rowIds[static_cast<size_t>(i)] = r.lobbyId;
    }
    if (g_status) {
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
    if (!fillDonor || !barDonor) {
        if (++g_buildAttempts >= 15 && !g_toldTheUser) {
            g_toldTheUser = true;
            UE_LOGE("server_browser_native: donors still absent after %d attempts "
                    "(ui_saveSlots_C.Image_0=%p ui_settings_C.scrollboxRoot=%p) -- NOT building",
                    g_buildAttempts, fillDonor, barDonor);
            ui::boot_warning_dialog::Arm(
                "Multivoid: the multiplayer screen could not be built.\n"
                "A required menu element was not found in this game build "
                "(ui_saveSlots.Image_0 / ui_settings.scrollboxRoot).\n"
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
    void* winOvl = winBox ? Spawn(L"Overlay", winBox) : nullptr;
    void* winFill = winOvl ? Spawn(L"Image", winOvl) : nullptr;
    void* col    = winOvl ? Spawn(L"VerticalBox", winOvl) : nullptr;
    if (!winBox || !winOvl || !winFill || !col) return false;
    U::SetSizeBoxWidth(winBox, kWindowW);
    U::SetSizeBoxHeight(winBox, kWindowH);
    // CLONE, do not author: DrawAs and Margin live INSIDE the brush, so a clone carries the
    // 9-slice for free and SetBrushDrawAs/SetBrushMargin (which do not exist in this build)
    // never come up. The handle at +0x70 is zeroed by CloneStyle.
    U::CloneStyle(winFill, P::off::UImage_Brush, fillDonor, P::off::UImage_Brush,
                  P::off::FSlateBrush_Size, kOneBrush, 1);
    E::SetWidgetVisibility(winFill, 0);
    if (void* s = U::AddChild(winOvl, winFill))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* s = U::AddChild(winOvl, col))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fn);
            f.Set<void*>(L"Content", winOvl);
            Call(winBox, f);
        }
    }
    if (void* s = U::AddChild(ovl, winBox))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        kCenter, kCenter);

    // (3) The content column: title, column headers, the list, the status line.
    {
        const std::string title = "MULTIPLAYER  -  " + sm::DisplayVersion();
        const std::wstring wtitle(title.begin(), title.end());
        g_title = AddText(col, wtitle.c_str(), 24, kAccent, kJustLeft, 0.f);
    }
    void* head = Spawn(L"HorizontalBox", col);
    if (head) {
        for (const Column& c : kColumns) AddText(head, c.title, 16, kDim, kJustLeft, c.weight);
        U::AddChild(col, head);
    }
    g_list = Spawn(L"ScrollBox", col);
    if (!g_list) return false;
    // The settings list's scrollbar treatment (section 7b): a server list is the long-list
    // case, and ui_saveSlots' own ScrollBox sets no bar style at all. NINE brushes.
    U::CloneStyle(g_list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    if (void* s = U::AddChild(col, g_list)) {
        auto* sz = reinterpret_cast<uint8_t*>(s) + P::off::UVerticalBoxSlot_Size;
        *reinterpret_cast<float*>(sz + P::off::FSlateChildSize_Value) = 1.f;
        *(sz + P::off::FSlateChildSize_SizeRule) = 1;  // Fill: the list takes the slack
        U::SetSlotAlign(s, P::off::UVerticalBoxSlot_HAlign, P::off::UVerticalBoxSlot_VAlign,
                        kFill, kFill);
    }
    g_status = AddText(col, L"", 16, kDim, kJustLeft, 0.f);

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

bool IsOpen() { return g_shown; }

void NotePointerMoved() { g_pointerMoved.store(true, std::memory_order_relaxed); }

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // Rebuild on a new menu instance (the old widgets died with it).
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_list = nullptr; g_status = nullptr; g_title = nullptr;
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
        g_prevEsc = esc;
        if (pressEdge) {
            Hide("ESC");
            return;
        }
    }

    selftest::Tick(g_scrimW, g_list);

    const uint64_t now = ::GetTickCount64();
    if (now - g_lastRefreshMs >= kRefreshMs) {
        g_lastRefreshMs = now;
        sm::Refresh();
        SyncRows();
    }
}

}  // namespace ui::server_browser_native
