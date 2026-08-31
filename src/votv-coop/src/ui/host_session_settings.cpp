// ui/host_session_settings.cpp -- see ui/host_session_settings.h for WHY this is its own
// window rather than three more rows on the hosting one.
//
// Built on ui/native_screen's kit and shaped after its two siblings in the same switcher.
// Where a construction fact is measured it lives in the kit's header and is not repeated.

#include "ui/host_session_settings.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/peer_identity.h"     // RandomBytes -- the CSPRNG the identity already uses
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"
#include "ui/host_window_native.h"      // step ONE -- what Back returns to
#include "ui/input_focus.h"
#include "ui/native_screen.h"
#include "ui/native_text_field.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace ui::host_session_settings {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace sm = coop::session_manager;
namespace TF = ui::native_text_field;
namespace HW = ui::host_window_native;
namespace cfg = coop::config;

using ue_wrap::FLinearColor;

// ---- layout --------------------------------------------------------------------------
// THE SAME WIDTH AS STEP ONE, on purpose: the player advances from one window to the next
// and a frame that changes size under them reads as a different program rather than as the
// next page. Shorter, because there is less to decide here.
constexpr float kWindowW  = 980.f;
// 470, and the number is the ONE deliberate compromise in this layout. The window keeps a
// CONSTANT height across both lock states rather than shrinking when the password block
// collapses -- a window that resizes as you click inside it is worse to use than one with
// space in it, and the browser's details panel records the same call. So this is sized for
// the LOCKED state (its hint line is the tallest content) and the unlocked state carries
// the difference as slack above the footer. Measured from the two captures, 2026-08-31:
// 520 left ~66 px of slack locked and ~170 unlocked, which read as a hole.
constexpr float kWindowH  = 470.f;
constexpr float kRowH     = 56.f;
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
constexpr float kFieldW   = 420.f;

const FLinearColor kPanel  = NS::Panel();
const FLinearColor kRowBg  = NS::RowBg();
const FLinearColor kRowSel = NS::RowSel();
const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kHover  = NS::Hover();
const FLinearColor kDim    = NS::Dim();
const FLinearColor kBad    = NS::Bad();

// ---- the two answers to the one question ----------------------------------------------
// The wording is the product surface and is fixed ONCE here. Each line says what the choice
// COSTS, because "Locked / Unlocked" alone asks the player to guess which one is which.
struct WhoMayJoin { const wchar_t* title; const wchar_t* detail; };
constexpr WhoMayJoin kWho[2] = {
    {L"Anyone can join",
     L"Your session is open to everyone who finds it."},
    {L"Password required",
     L"Only players you give the password to."},
};

// ---- the generated password -----------------------------------------------------------
//
// THE ALPHABET HAS NO I, l, 1, O, 0. This value is read aloud over voice chat and typed
// back by hand, so the characters that cost a retry are the ones that look like each other
// in a UI font. Twenty-six letters minus I and O, ten digits minus 0 and 1, and lower case
// dropped whole because "was that a capital?" is the same retry.
constexpr char kPwAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int  kPwAlphabetN  = 32;   // sizeof - 1, stated so the modulo claim below is checkable
constexpr int  kPwLen        = 10;   // 10 x log2(32) = 50 bits

static_assert(sizeof(kPwAlphabet) - 1 == kPwAlphabetN, "the alphabet and its size must agree");
// 256 % 32 == 0, so `byte % 32` is EXACTLY uniform and the usual rejection-sampling dance
// is not merely skipped, it is unnecessary. This is the one arithmetic that makes modulo
// safe here; a 33-character alphabet would silently bias the first character class.
static_assert(256 % kPwAlphabetN == 0, "modulo would bias the alphabet");

// Empty on failure -- and the caller must treat that as a REFUSAL, never as a reason to
// reach for `rand()`. A predictable lobby password is worse than an open lobby, because the
// padlock tells the host they are protected.
std::string GeneratePassword() {
    unsigned char raw[kPwLen];
    if (!coop::net::peer_identity::RandomBytes(raw, sizeof(raw))) {
        UE_LOGE("host_session_settings: the system RNG refused -- NOT generating a password "
                "(a guessable one would be worse than none, because the lock would still "
                "say you are protected)");
        return {};
    }
    std::string s(kPwLen, '\0');
    for (int i = 0; i < kPwLen; ++i) s[static_cast<size_t>(i)] = kPwAlphabet[raw[i] % kPwAlphabetN];
    return s;
}

// ---- state (GAME THREAD ONLY unless marked) ------------------------------------------
void* g_menu     = nullptr;
void* g_switcher = nullptr;
void* g_root     = nullptr;
void* g_scrim    = nullptr;
void* g_backBtn  = nullptr;
void* g_hostBtn  = nullptr;
void* g_status   = nullptr;
void* g_pwBlock  = nullptr;   // the label + field + hint, shown only while locked
void* g_pwHint   = nullptr;
void* g_recapName = nullptr;
void* g_recapWorld = nullptr;
void* g_recapConn  = nullptr;
void* g_whoBg[2]    = {nullptr, nullptr};
void* g_whoTitle[2] = {nullptr, nullptr};

TF::Field* g_pwField = nullptr;

int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;
int     g_buildAttempts = 0;
bool    g_toldTheUser   = false;

// WHAT STEP ONE DECIDED. Carried, never re-derived: there is exactly one place in the tree
// that reads the save list and the connection rows, and it is the window the player just
// used (see the header).
sm::SaveChoice g_choice;
std::string    g_name;
int            g_connMode = 0;

bool g_locked = false;
int  g_hoverWho = -1;

// The status last WRITTEN -- BOTH the string and the colour, because this line changes
// colour on a refusal and a cache that remembers only the text would suppress the repaint
// that turns it red. Module-level and cleared with the widget on the menu-instance edge: a
// function-local static outlives the UTextBlock it caches for and leaves the line
// permanently blank on the second visit (the defect the hosting window's own status line
// shipped with, found by the 2026-08-31 audit).
std::wstring g_lastStatus;
FLinearColor g_lastStatusColor{};
bool         g_haveStatus = false;

bool g_prevLmb = false, g_lmbPrimed = false;
bool g_prevEsc = false, g_escPrimed = false;

// Cross-thread open/close intent, the same shape both siblings use -- plus a PAYLOAD, which
// they do not have.
//
// AND THE PAYLOAD IS WHY THERE IS A MUTEX. The siblings' intents are bare timestamps, so an
// atomic says everything there is to say; this one carries a `SaveChoice` and two strings,
// and an atomic timestamp published beside a plain struct orders nothing -- the game thread
// could read a half-written `std::string` and the failure would be a crash in the recap
// line, nowhere near the writer. Today the only caller is on the game thread anyway, which
// is exactly the condition under which such a race stays invisible until the day someone
// adds a second one.
struct PendingOpen {
    sm::SaveChoice choice;
    std::string    name;
    int            connMode = 0;
};
PendingOpen           g_pending;
std::mutex            g_pendingMu;
std::atomic<uint64_t> g_wantOpenMs{0};
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs = 20000;

// ---- helpers ---------------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    return coop::text::FromUtf8Lossy(s.data(), s.size());
}

void SetText(void* block, const std::wstring& t, const FLinearColor& col) {
    if (!block) return;
    E::SetWidgetText(block, t.c_str());
    E::SetTextBlockColorDispatch(block, col);
}

// EDGE-GATED, like every status line in these screens: `SetWidgetText` is two dispatches
// plus an FText, and this one changes only when a host attempt finishes or a refusal fires.
void SetStatus(const std::wstring& t, const FLinearColor& col) {
    if (!g_status) return;
    if (g_haveStatus && t == g_lastStatus && col.R == g_lastStatusColor.R &&
        col.G == g_lastStatusColor.G && col.B == g_lastStatusColor.B)
        return;
    g_lastStatus = t;
    g_lastStatusColor = col;
    g_haveStatus = true;
    SetText(g_status, t, col);
}

// One selectable row: SizeBox -> Overlay -> [ Image (the HIT TARGET and the selection
// fill), HorizontalBox of text ]. No UButton, for the reason the browser's rows record: a
// bare UImage is what we can paint, and a UButton would add a press visual we would then
// have to suppress.
struct Row { void* box; void* bg; void* a; void* b; };

Row BuildRow(void* parent, float wA, float wB) {
    Row r{};
    r.box = NS::Spawn(L"SizeBox", parent);
    if (!r.box) return r;
    U::SetSizeBoxHeight(r.box, kRowH);
    void* ovl = NS::Spawn(L"Overlay", r.box);
    if (!ovl) return Row{};
    r.bg = NS::Spawn(L"Image", ovl);
    if (!r.bg) return Row{};
    U::SetImageTintRaw(r.bg, kRowBg);
    E::SetWidgetVisibility(r.bg, 0);   // Visible: it is the hit target
    if (void* s = U::AddChild(ovl, r.bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kFill);
    if (void* hb = NS::Spawn(L"HorizontalBox", ovl)) {
        r.a = NS::AddText(hb, L"", 18, kText, NS::kJustLeft, wA);
        r.b = NS::AddText(hb, L"", 15, kDim,  NS::kJustLeft, wB);
        if (void* s = U::AddChild(ovl, hb)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            NS::kFill, NS::kCenter);
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UOverlaySlot_Padding);
            pad[0] = 12.f; pad[1] = 0.f; pad[2] = 12.f; pad[3] = 0.f;
        }
    }
    U::SetContent(r.box, ovl);
    if (!r.bg || !r.a) return Row{};
    U::AddChild(parent, r.box);
    return r;
}

// A titled framed section, the shape `server_browser_panels::SectionBody` uses. Kept local
// rather than lifted into the kit: this is the SECOND caller, and the kit's own rule
// (OPUS_48_DISCIPLINE:196) is no new shared framework before three working cases.
void* SectionBody(void* parent, const wchar_t* title) {
    void* box = NS::AddFramedBox(parent, kPanel, kBorderPx);
    void* col = box ? NS::Spawn(L"VerticalBox", box) : nullptr;
    if (!box || !col) return nullptr;
    if (void* s = U::AddChild(box, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kTop);
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, 10.f, 8.f, 10.f, 8.f);
    }
    if (title) NS::AddText(col, title, 18, kAccent, NS::kJustLeft, 0.f);
    NS::AddVFill(parent, box, 0.f, NS::kFill, NS::kTop);
    return col;
}

void RepaintChoices() {
    for (int i = 0; i < 2; ++i) {
        if (!g_whoBg[i]) continue;
        // Style doc section 4: selection is a FILL and hover is a TEXT colour. Two
        // independent channels; porting ImGui's HeaderHovered here would look foreign.
        U::SetImageTint(g_whoBg[i], (g_locked ? 1 : 0) == i ? kRowSel : kRowBg);
        E::SetTextBlockColorDispatch(g_whoTitle[i], g_hoverWho == i ? kHover : kText);
    }
    // THE PASSWORD BLOCK IS COLLAPSED, NOT HIDDEN. Collapsed (1) takes no space, so the
    // window closes up around it; Hidden (2) would keep a field-sized hole. This is the one
    // place in these screens where the difference is wanted the other way round from the
    // row-alignment case the padlock cell records.
    if (g_pwBlock) E::SetWidgetVisibility(g_pwBlock, g_locked ? 0 : 1);
}

// TURNING THE LOCK ON MINTS A PASSWORD IMMEDIATELY, which is what the user asked for:
// "если жмет на замок то пароль сразу появляется сгенерированный". A lock that opened an
// empty box would make the player invent a password at the worst possible moment -- the one
// where they are in a hurry to start a game -- and that is how "1234" happens.
//
// It mints only when the box is EMPTY, so toggling the lock off and on again does not throw
// away a value the player just typed or already told a friend.
void SetLocked(bool locked) {
    if (locked == g_locked) return;
    g_locked = locked;
    if (g_locked && TF::Text(g_pwField).empty()) {
        const std::string pw = GeneratePassword();
        if (pw.empty()) {
            // FAIL CLOSED. The RNG refused, so the lock does not go on: a padlock with no
            // secret behind it is the false promise this project has already shipped once.
            g_locked = false;
            SetStatus(L"Could not generate a password on this system -- the lock stays off.",
                      kBad);
            RepaintChoices();
            return;
        }
        TF::SetText(g_pwField, pw);
    }
    if (!g_locked) TF::Blur(g_pwField);   // a collapsed field must not keep the keyboard
    SetStatus(L"", kText);
    RepaintChoices();
}

}  // namespace

// ============================ build ====================================================
namespace {

bool BuildScreen(void* switcher) {
    void* saveSlots = NS::SwitcherChild(switcher, L"ui_saveSlots_C");
    void* backDonor = NS::DonorField(saveSlots, L"button_back");
    if (!backDonor) {
        // Fail CLOSED and retry. The browser owns the loud player-facing alarm for a missing
        // donor; this screen is two doors behind it, so a second dialog would only stack.
        if (++g_buildAttempts == 15) {
            g_toldTheUser = true;
            UE_LOGE("host_session_settings: ui_saveSlots_C.button_back absent after %d "
                    "attempts -- NOT building", g_buildAttempts);
        }
        return false;
    }

    NS::WindowShell shell;
    if (!NS::BuildWindowShell(switcher, kWindowW, kWindowH,
                              L"Multivoid  -  Session settings", shell))
        return false;
    void* col = shell.column;
    g_scrim   = shell.scrim;

    // ---- what step one decided, so the player can commit to it without going back -------
    // Read-only. It is not a second place to change these values -- Back is.
    if (void* recap = SectionBody(col, L"Your session:")) {
        g_recapName  = NS::AddText(recap, L"", 20, kText, NS::kJustLeft, 0.f);
        g_recapWorld = NS::AddText(recap, L"", 16, kDim,  NS::kJustLeft, 0.f);
        g_recapConn  = NS::AddText(recap, L"", 16, kDim,  NS::kJustLeft, 0.f);
        U::SetClipping(g_recapName, 1);
        U::SetClipping(g_recapWorld, 1);
        U::SetClipping(g_recapConn, 1);
    }

    NS::AddText(col, L"WHO MAY JOIN", 16, kAccent, NS::kJustLeft, 0.f);
    for (int i = 0; i < 2; ++i) {
        Row r = BuildRow(col, 0.42f, 0.58f);
        g_whoBg[i]    = r.bg;
        g_whoTitle[i] = r.a;
        SetText(r.a, kWho[i].title,  kText);
        SetText(r.b, kWho[i].detail, kDim);
        if (!r.bg || !r.a) return false;
    }

    // ---- the password block, collapsed until the lock goes on ---------------------------
    // ONE CONTAINER, so the whole block appears and disappears together. The field itself
    // cannot be toggled -- `native_text_field` deliberately exposes no widget (RULE 2, its
    // header) -- and it must not be Ticked while it is off screen either, because its hit
    // test is by GEOMETRY and a collapsed widget keeps the rect it last painted with.
    g_pwBlock = NS::Spawn(L"VerticalBox", col);
    if (!g_pwBlock) return false;
    NS::AddVFill(col, g_pwBlock, 0.f, NS::kFill, NS::kTop);
    NS::AddText(g_pwBlock, L"Password", 16, kAccent, NS::kJustLeft, 0.f);
    // IN A ROW OF ITS OWN, so `kFieldW` actually applies. The field is a SizeBox with a
    // width override, and a VerticalBox slot fills horizontally by default -- so parenting
    // it straight to the column stretched it to the full 968 px and the constant did
    // nothing (visible in the first capture: a full-width banner around ten characters).
    // A HorizontalBox slot is auto-sized, which is what lets the override win.
    void* pwRow = NS::Spawn(L"HorizontalBox", g_pwBlock);
    if (!pwRow) return false;
    NS::AddVFill(g_pwBlock, pwRow, 0.f, NS::kFill, NS::kTop);
    g_pwField = TF::Create(pwRow, L"password", 48, kFieldW);
    if (!g_pwField) return false;
    g_pwHint = NS::AddText(g_pwBlock,
                           L"Anyone with this can join. Give it out the way you would give "
                           L"out an invite link.",
                           15, kDim, NS::kJustLeft, 0.f);
    if (g_pwHint) U::SetAutoWrapText(g_pwHint, true);
    E::SetWidgetVisibility(g_pwBlock, 1);   // Collapsed: the lock starts off

    // A SPACER WITH ALL THE SLACK, so the footer sits at the bottom of the window rather
    // than floating under the last control. The same trick the input windows use, and it
    // costs one widget; the alternative -- a Fill slot on the footer -- makes the footer's
    // own height the window's leftover, which is exactly the arithmetic that pushed step
    // one's buttons off the frame twice in one day.
    if (void* spacer = NS::Spawn(L"Spacer", col)) NS::AddVFill(col, spacer, 1.f, NS::kFill, NS::kFill);

    // FOOTER: Back at the LEFT, Host at the RIGHT, status between. Bottom-left is the way
    // out and bottom-right is the commit, in every native VOTV window that has both (style
    // doc section 5, gap S7). No bordered strip around them -- VOTV frames CONTENT, never a
    // row of buttons, and the strip is what the user read as a second window edge.
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        g_backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        g_status  = NS::AddText(footRow, L"", 16, kText, NS::kJustCenter, 1.f);
        g_hostBtn = NS::BuildButton(footRow, backDonor, L"Host", NS::kBtnFontPx);
        if (!g_backBtn || !g_hostBtn) return false;
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft,  NS::kCenter);
        NS::SetHSlot(NS::SlotOf(g_hostBtn), 0.f, NS::kRight, NS::kCenter);
        if (void* s = NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    g_root = shell.root;

    // ATTACH AT BIRTH, NOT AT FIRST Show(). An unattached widget tree is GC food: it renders
    // until the next collection and then vanishes, and `AddChild` on the dead object returns
    // null. Measured on the MULTIPLAYER button, 2026-08-30; both siblings attach here for the
    // same reason. `AddToRoot` is the wrong tool -- a switcher child is reachable from the
    // menu, which is the reference we actually want.
    void* slot = U::AddChild(switcher, g_root);
    g_ourIndex = U::IndexOfChild(switcher, g_root);
    if (g_ourIndex < 0) {
        UE_LOGE("host_session_settings: built the session window but could NOT place it in "
                "the menu switcher (AddChild slot=%p, GetChildIndex=-1) -- it cannot be shown "
                "this menu", slot);
        // AND THE FIELD GOES WITH IT -- Release, not Destroy: the tree we just built is
        // orphaned, so dispatching RemoveChild into it is a call against whatever reuses the
        // slot. Clearing `g_root` alone would leave the heap Field alive and in the module's
        // live list, and the next tick would rebuild everything and leak another one (the
        // input screens' own audit finding, 2026-08-31).
        TF::Release(g_pwField);
        g_pwField = nullptr;
        g_root = nullptr;
        return false;
    }
    UE_LOGI("host_session_settings: screen built (root=%p index=%d) after %d attempt(s)",
            g_root, g_ourIndex, g_buildAttempts + 1);
    return true;
}

// ============================ actions ==================================================

void Hide(const char* why);

// HOST -- the ONE host action in the tree, and the only place it is called from since the
// hosting flow gained its second step. No hosting rule is authored here: if one must
// change, it changes in `session_manager`, once.
void DoHost() {
    const std::string pw = g_locked ? TF::Text(g_pwField) : std::string();
    if (g_locked && pw.empty()) {
        // A LOCK WITH NO SECRET IS THE FALSE PROMISE, so it is refused here rather than
        // announced. The player emptied the box themselves; the way out is to type something
        // or to choose "Anyone can join".
        SetStatus(L"Type a password, or choose \"Anyone can join\".", kBad);
        return;
    }

    // PERSISTED BEFORE THE ATTEMPT, so a host who has told their friends a value keeps it
    // whether or not this particular start succeeds -- and so the next session opens with
    // the lock the way they left it ("Generate for them on first, then they can change it to
    // whatever they want at any moment").
    cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_password, pw.c_str());
    cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_locked, g_locked ? "1" : "0");

    const bool accepted = sm::HostWithSave(g_choice, g_name, g_locked, /*playersMax=*/4,
                                           /*directConnection=*/g_connMode == 1,
                                           /*hideFromBrowser=*/false,
                                           /*lanOnly=*/g_connMode == 2);
    // THE PASSWORD IS NEVER LOGGED. It is the one value in this window that is a secret, and
    // a log line is the easiest place in the whole program for it to end up in a screenshot.
    UE_LOGI("host_session_settings: HOST %s -- world=%s conn=%d locked=%d name='%s'",
            accepted ? "accepted" : "REFUSED (another action in flight)",
            g_choice.newGame ? "<new game>" : g_choice.slot.c_str(), g_connMode,
            g_locked ? 1 : 0, g_name.c_str());
    if (!accepted) {
        // The window STAYS OPEN on a refusal, for the reason step one records: a screen that
        // closes on failure is how "nothing told about the session being DEAD" happened --
        // the only surface showing the reason was the one that had just gone away.
        SetStatus(L"Busy -- another host or join is already starting.", kBad);
        return;
    }
    SetStatus(L"Starting...", kText);
}

// BACK RETURNS TO STEP ONE, not to the main menu -- this window is only ever reached from
// the hosting window, and a two-step flow whose Back exits the flow entirely would make the
// player redo the world choice to change one connection mode.
//
// Restoring the switcher index is NOT enough on its own: the hosting window tracks its own
// `g_shown` and reconciled itself closed when we took the switcher, so it would consider
// itself hidden while its pixels were on screen. Asking it to `Open()` is the one call that
// puts the index and that flag back in agreement -- the same rule the input windows follow
// with the browser.
void BackToHostWindow() {
    Hide("BACK");
    HW::Open();
}

void PollChrome() {
    if (!ui::input_focus::IsOurWindowForeground()) return;

    // ESC. A FOCUSED FIELD OWNS IT FIRST -- the field turns Escape into "leave the field",
    // and this poll reads the PHYSICAL key, so swallowing the message at the WndProc seam
    // would not stop this edge. One press must not both blur and go back.
    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
    const bool escEdge = g_prevEsc && !esc;
    g_prevEsc = esc;
    if (escEdge) {
        if (TF::AnyFocused()) return;
        BackToHostWindow();
        return;
    }

    // ENTER in the password field HOSTS. The key that ends typing is the one that acts,
    // which is what every text field a player has ever used does.
    if (g_locked && TF::ConsumeSubmit(g_pwField)) { DoHost(); return; }

    const bool lmb = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = lmb; return; }
    const bool released = g_prevLmb && !lmb;
    g_prevLmb = lmb;
    if (!released) return;

    // TWO MECHANISMS, BECAUSE THERE ARE TWO KINDS OF WIDGET (measured; `native_screen.h`).
    // A real UButton answers `IsHovered`; a hand-built UImage does not and must come through
    // geometry. Unifying them was tried on 2026-08-30 and turned a passing verdict into a
    // failure.
    if (g_backBtn && E::WidgetIsHovered(g_backBtn)) { BackToHostWindow(); return; }
    if (g_hostBtn && E::WidgetIsHovered(g_hostBtn)) { DoHost(); return; }
    for (int i = 0; i < 2; ++i)
        if (g_whoBg[i] && NS::CursorOverWidget(g_whoBg[i])) { SetLocked(i == 1); return; }
}

void UpdateHover() {
    const int prev = g_hoverWho;
    g_hoverWho = -1;
    // These rows sit in the content column, OUTSIDE any ScrollBox, but they are still
    // hand-built UImages -- and `IsHovered()` reads 0 on one of those whether or not it is
    // inside a scroll container (measured twice, 2026-08-29 and 2026-08-30). Geometry is the
    // only mechanism that answers for them.
    for (int i = 0; i < 2; ++i)
        if (g_whoBg[i] && NS::CursorOverWidget(g_whoBg[i])) { g_hoverWho = i; break; }
    if (g_hoverWho != prev) RepaintChoices();
}

// ============================ lifecycle ================================================

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    g_priorIndex = U::SwitcherIndex(g_switcher);
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    g_hoverWho  = -1;
    g_escPrimed = false;
    g_lmbPrimed = false;
    g_lastStatus.clear();
    SetStatus(L"", kText);

    SetText(g_recapName, Widen(g_name), kText);
    SetText(g_recapWorld,
            g_choice.newGame ? std::wstring(L"World: a new game")
                             : L"World: " + Widen(g_choice.slot),
            kDim);
    SetText(g_recapConn,
            std::wstring(L"Connection: ") +
                (g_connMode == 1 ? L"direct (you forward the port)"
                                 : g_connMode == 2 ? L"LAN only" : L"auto"),
            kDim);

    // THE LOCK AND THE PASSWORD COME BACK THE WAY THEY WERE LEFT. A host who set one last
    // week should not have to re-read it off a friend's screen.
    const std::string saved = cfg::ResolveString(::coop::config_registry::rows::net_lobby_password);
    TF::SetText(g_pwField, saved);
    // Forced through the mint path rather than assigned, so an ini that says locked=1 with
    // an empty password still ends up with a real secret instead of a padlock over nothing.
    g_locked = false;
    RepaintChoices();
    if (cfg::ResolveFlag(::coop::config_registry::rows::net_lobby_locked)) SetLocked(true);

    UE_LOGI("host_session_settings: shown (index %d -> %d; locked=%d)",
            g_priorIndex, g_ourIndex, g_locked ? 1 : 0);
}

void Hide(const char* why) {
    if (!g_shown) return;
    g_shown = false;
    TF::Blur(g_pwField);
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == g_ourIndex && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("host_session_settings: hidden (%s; index was %d, ours %d)", why, now, g_ourIndex);
}

bool Armed() {
    static const bool s = cfg::ResolveFlag(::coop::config_registry::rows::browser_native);
    return s;
}

}  // namespace

// ============================ public API ===============================================

void Open(const coop::session_manager::SaveChoice& choice, const std::string& serverName,
          int connMode) {
    // The intent CARRIES the decision. Re-deriving it on the game thread would mean this
    // window reading the save list itself, which is the duplication the header forbids.
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pending.choice   = choice;
        g_pending.name     = serverName;
        g_pending.connMode = connMode;
    }
    g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

void Close() {
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    g_wantClose.store(true, std::memory_order_relaxed);
}

bool IsOpen() { return g_shown; }

void* LockRow()    { return g_whoBg[1]; }
void* BackButton() { return g_backBtn; }
bool  Locked()     { return g_locked; }

int PasswordLength() {
    // The FIELD, not the ini row: what the self-check has to prove is that clicking the
    // lock put a value in front of the player THIS session, and a row read would answer
    // yes on a rig whose ini already carried one from a previous run.
    return static_cast<int>(TF::Text(g_pwField).size());
}

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        // RELEASE, NOT DESTROY. The widgets died with the menu instance, so the field must
        // unhook its focus and free its handle WITHOUT dispatching RemoveChild into a tree
        // that no longer exists.
        TF::Release(g_pwField);
        g_pwField = nullptr;
        g_root = nullptr; g_scrim = nullptr; g_status = nullptr;
        g_backBtn = nullptr; g_hostBtn = nullptr;
        g_pwBlock = nullptr; g_pwHint = nullptr;
        g_recapName = g_recapWorld = g_recapConn = nullptr;
        for (int i = 0; i < 2; ++i) { g_whoBg[i] = nullptr; g_whoTitle[i] = nullptr; }
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0;
        g_lastStatus.clear();   // the widget it cached is gone with the menu
    }

    if (!g_root) {
        // BACKED OFF once it is hopeless, because the retry is not free: each attempt costs
        // a `SwitcherChild` walk (a ChildCount plus a ClassNameOf per child -- an engine call
        // and a wstring EACH) plus a donor lookup, at ~117 menu ticks a second, forever, on
        // exactly the path a version migration lands on.
        if (g_toldTheUser) {
            static uint64_t sNextTryMs = 0;
            const uint64_t now = ::GetTickCount64();
            if (now < sNextTryMs) return;
            sNextTryMs = now + 1000;
        }
        if (!BuildScreen(switcher)) {
            if (++g_buildAttempts >= 15) g_toldTheUser = true;
            static bool sSaidSo = false;
            if (!sSaidSo && g_wantOpenMs.load(std::memory_order_relaxed)) {
                sSaidSo = true;
                UE_LOGE("host_session_settings: a Next request is pending but the screen will "
                        "not build (attempt %d) -- the player pressed Next, the hosting window "
                        "closed, and nothing opened", g_buildAttempts);
            }
            return;
        }
    }

    if (g_wantClose.exchange(false, std::memory_order_relaxed)) Hide("requested");
    const uint64_t want = g_wantOpenMs.load(std::memory_order_relaxed);
    if (want) {
        g_wantOpenMs.store(0, std::memory_order_relaxed);
        const uint64_t age = ::GetTickCount64() - want;
        if (age <= kIntentTtlMs) {
            {
                std::lock_guard<std::mutex> lk(g_pendingMu);
                g_choice   = g_pending.choice;
                g_name     = g_pending.name;
                g_connMode = g_pending.connMode;
            }
            // STEP ONE CLOSES FIRST AND SYNCHRONOUSLY. Both screens are children of one
            // switcher and each records the index it replaces, so opening on top of a live
            // sibling makes THAT sibling's index the one this window would restore -- and its
            // Back would then walk back into us. See `host_window_native::CloseNow`.
            HW::CloseNow();
            Show();
        } else {
            UE_LOGW("host_session_settings: an open request expired unconsumed after %llu ms "
                    "-- no main-menu tick arrived to show the window",
                    static_cast<unsigned long long>(age));
        }
    }

    if (!g_shown) return;

    // Reconcile against the LIVE index rather than asserting ours: a sibling screen (or the
    // game's own ESC path) can navigate away, and if it did we were closed, whoever did it.
    if (U::SwitcherIndex(g_switcher) != g_ourIndex) {
        TF::Blur(g_pwField);
        g_shown = false;
        return;
    }

    // ONLY WHILE THE BLOCK IS ON SCREEN. `Tick` takes focus when the pointer is pressed
    // inside the field's own rect BY GEOMETRY, and a collapsed widget keeps the rect it last
    // painted with -- so ticking a hidden field would let a click on the row above it steal
    // the keyboard into a box the player cannot see.
    if (g_locked) TF::Tick(g_pwField);
    UpdateHover();
    PollChrome();
}

}  // namespace ui::host_session_settings
