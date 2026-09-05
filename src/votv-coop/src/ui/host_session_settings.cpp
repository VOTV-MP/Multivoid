// ui/host_session_settings.cpp -- the second hosting window (the header says why it is its own
// window). Built on ui/native_screen's kit, shaped after its two siblings in the same switcher.

#include "ui/host_session_settings.h"

#include "coop/config/config.h"
#include "coop/text/utf8_codec.h"   // the server name is sanitised where it is authored
#include "coop/config/config_registry.h"
#include "coop/net/peer_identity.h"     // RandomBytes -- the CSPRNG the identity already uses
#include "coop/session/host_mode.h"
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"
#include "ui/host_session_choices.h"     // the two-row selector these questions share
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
#include <cmath>
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
namespace HC = ui::host_session_choices;

using ue_wrap::FLinearColor;

// ---- layout ----
// The same width as step one, so the next page reads as the next page; shorter, less to decide.
constexpr float kWindowW  = 980.f;
// One height across both lock states (a window that resizes as you click inside it is worse than
// one with slack): sized for the locked state, the tallest cell, with ~57 px of margin over the
// measured need; ReportFit logs an error the moment a cell overflows.
constexpr float kWindowH  = 690.f;
constexpr float kRowH     = 56.f;
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
constexpr float kFieldW   = 420.f;
// The server name's bound in bytes (what the field counts and what travels): 24 codepoints at the
// 4-byte worst case, about 96 in Latin; a name, not a paragraph, drawn in one browser row.
constexpr int32_t kNameMaxBytes = 96;

const FLinearColor kPanel  = NS::Panel();
const FLinearColor kRowBg  = NS::RowBg();
const FLinearColor kRowSel = NS::RowSel();
const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kHover  = NS::Hover();
const FLinearColor kDim    = NS::Dim();
const FLinearColor kBad    = NS::Bad();
const FLinearColor kAmber  = NS::Amber();   // the DIRECT/LAN caveat on the hint line

// ---- the two answers to the one question ----
// Each line says what the choice costs. Typed as the selector's own `Answer`, not a look-alike.
constexpr HC::Answer kWho[2] = {
    {L"Anyone can join",
     L"Your session is open to everyone who finds it."},
    {L"Password required",
     L"Only players you give the password to."},
};

// ---- the second question: can anyone find it ----
// The same control as the ~ menu's "Show in server browser", decided at creation, before the
// announce goes out, with the same positive polarity. Rows, not a checkbox: they must state the
// truth on AUTOMATIC (always listed; the master is the only rendezvous) and be movable on DIRECT.
constexpr HC::Answer kVis[2] = {
    {L"Show in server browser",
     L"Anyone can find your game in the list."},
    {L"Hidden",
     L"Only friends you give your address to."},
};

// ---- the generated password ----
// No I, l, 1, O or 0 and no lower case: the value is read aloud and typed back, so look-alikes cost
// a retry.
constexpr char kPwAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int  kPwAlphabetN  = 32;   // sizeof - 1, stated so the modulo claim below is checkable
// Six characters = 30 bits: ~six GPU-hours against lobby_password.h's 200 000 PBKDF2 iterations.
// Safe only because a proof is sent solely to a host the joiner bound to an identity given in
// advance or typed themselves (peer_admission.cpp): that binding gate must not be relaxed on the
// strength of this constant, and a typed-address typo hands the password to whoever answers.
constexpr int  kPwLen        = 6;

static_assert(sizeof(kPwAlphabet) - 1 == kPwAlphabetN, "the alphabet and its size must agree");
// 256 % 32 == 0, so `byte % 32` is exactly uniform; a 33-character alphabet would bias the modulo.
static_assert(256 % kPwAlphabetN == 0, "modulo would bias the alphabet");

// Empty on failure, which the caller treats as a refusal, never a reason to fall back to rand(): a
// predictable password under a lit padlock is worse than an open lobby.
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

// ---- state (game thread only unless marked) ----
void* g_menu     = nullptr;
void* g_switcher = nullptr;
void* g_root     = nullptr;
void* g_box      = nullptr;   // the centred window frame (NOT g_root, which is full-screen)
void* g_scrim    = nullptr;
void* g_backBtn  = nullptr;
void* g_hostBtn  = nullptr;
void* g_status   = nullptr;
void* g_pwBlock  = nullptr;   // the label + field + hint, shown only while locked
void* g_pwHint   = nullptr;
void* g_recapWorld = nullptr;
void* g_recapConn  = nullptr;
// The questions, each a host_session_choices::Selector; `chosen` carries the answer, so there are
// no parallel booleans.
HC::Selector g_who;   // 0 = anyone may join, 1 = a password is required
HC::Selector g_vis;   // 0 = listed in the server browser, 1 = hidden

// Read through these, never off `chosen`: the index-to-meaning mapping lives in one place.
bool IsLocked() { return g_who.chosen == 1; }
bool IsListed() { return g_vis.chosen == 0; }
void* g_visHint     = nullptr;   // why the choice is fixed, on the modes where it is

TF::Field* g_pwField   = nullptr;
TF::Field* g_nameField = nullptr;   // the server name, editable here (see the recap block)

// Every early return from the build past the first TF::Create comes through here, so no path can
// forget a release.
void ReleaseFields() {
    TF::Release(g_nameField); g_nameField = nullptr;
    TF::Release(g_pwField);   g_pwField   = nullptr;
}

int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;
int     g_buildAttempts = 0;
bool    g_toldTheUser   = false;

// What step one decided, carried rather than re-derived: one place in the tree reads the save list
// and the connection rows.
sm::SaveChoice g_choice;
std::string    g_name;
int            g_connMode = 0;


// The last overflow ReportFit reported, so the line is logged on change, not per tick.
float g_fitLast = -9999.f;

// The fit probe's dirty flag. Content taller than kWindowH pushes the footer past the frame with
// nothing clipping, so the height is measured, not eyeballed: on the tick (on the frame the index
// changes every rect reads 0x0) and only when the layout moved (the lock toggle, a fresh showing),
// since a measurement is six dispatches and eight allocations at the menu's ~117 Hz.
bool g_fitDirty = true;

// The connection mode the visibility answer was chosen under; -1 = never, so the first Show()
// derives (see Show()).
int g_visModeWhenChosen = -1;

void ReportFit() {
    if (!g_fitDirty) return;
    ue_wrap::FVector2D rtl{}, rsz{}, btl{}, bsz{};
    if (!g_box || !g_hostBtn) return;
    if (!U::WidgetScreenRect(g_box, rtl, rsz) || rsz.Y < 1.f) return;
    if (!U::WidgetScreenRect(g_hostBtn, btl, bsz) || bsz.Y < 1.f) return;
    const float frameBottom  = rtl.Y + rsz.Y;
    const float footerBottom = btl.Y + bsz.Y;
    const float overflow     = footerBottom - frameBottom;
    // Cleared only after both rects read non-degenerate: on the switcher-change frame every rect is
    // 0x0, and clearing on entry would spend the flag on the one tick that cannot answer.
    g_fitDirty = false;
    if (g_fitLast > -9000.f && std::fabs(overflow - g_fitLast) < 1.f) return;
    g_fitLast = overflow;
    if (overflow > 0.f) {
        UE_LOGE("host_session_settings: [fit] FOOTER OUTSIDE THE FRAME by %.0f px "
                "(frame ends %.0f, Host button ends %.0f; locked=%d conn=%d) -- kWindowH is "
                "too small for this cell",
                overflow, frameBottom, footerBottom, IsLocked() ? 1 : 0, g_connMode);
    } else {
        UE_LOGI("host_session_settings: [fit] footer inside the frame, %.0f px of slack "
                "(locked=%d conn=%d)", -overflow, IsLocked() ? 1 : 0, g_connMode);
    }
}

// Listed by default in both modes. There is no LAN-only mode: one that forced listed=false and
// refused non-private remotes is DIRECT + Hidden, which this selector already expresses
// (coop/session/host_mode.h).

// Can this connection mode's visibility be moved at all? DIRECT only; see kVis.
bool VisibilityIsEditable() { return g_connMode == 1; }

// Both modes start listed, so a per-mode predicate could only ever return true; the default is
// stated where it is used.
constexpr bool kListedByDefault = true;

// The status last written, string and colour (the line turns red on a refusal, so a text-only cache
// would suppress that repaint). Module-level and cleared on the menu-instance edge: a
// function-local static would outlive the UTextBlock it caches for.
std::wstring g_lastStatus;
FLinearColor g_lastStatusColor{};
bool         g_haveStatus = false;
// The hint's cache, the same discipline.
int          g_hintFor = -2;
// Has this opening attempted a host? The status string is global and outlives the action.
bool         g_sawHostAttempt = false;
std::string  g_lastHostStatus;

bool g_prevLmb = false, g_lmbPrimed = false;
bool g_prevEsc = false, g_escPrimed = false;

// Cross-thread open intent, the siblings' shape plus a payload; the payload is why there is a
// mutex: an atomic timestamp beside a plain struct orders nothing, and the game thread could read a
// half-written string.
struct PendingOpen {
    sm::SaveChoice choice;
    std::string    name;
    int            connMode = 0;
};
PendingOpen           g_pending;
std::mutex            g_pendingMu;
std::atomic<uint64_t> g_wantOpenMs{0};
constexpr uint64_t kIntentTtlMs = 20000;

// ---- helpers ----

std::wstring Widen(const std::string& s) {
    return coop::text::FromUtf8Lossy(s.data(), s.size());
}

void SetText(void* block, const std::wstring& t, const FLinearColor& col) {
    if (!block) return;
    E::SetWidgetText(block, t.c_str());
    E::SetTextBlockColorDispatch(block, col);
}

// Edge-gated: SetWidgetText is two dispatches plus an FText, and this changes only on a host
// attempt or a refusal.
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

// The selectable rows live in ui/host_session_choices, one body for the two questions here and the
// connection-mode one in the hosting window; a bare UImage is the hit target, since a UButton would
// add a press visual to suppress.

// A titled framed section, server_browser_panels::SectionBody's shape; local because this is the
// second caller, and the kit takes no shared framework before three.
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
    // Both style channels live in host_session_choices::Repaint (selection and hover are
    // independent).
    HC::Repaint(g_who);
    // Collapsed (1), not Hidden (2): collapsed takes no space, so the window closes up around it;
    // hidden would keep a field-sized hole.
    if (g_pwBlock) E::SetWidgetVisibility(g_pwBlock, IsLocked() ? 0 : 1);

    // The visibility rows, dimmed whole when the mode decides them: the selection fill stays (it
    // still states what will happen), the white goes, so the section reads as information rather
    // than a control that ignores clicks.
    const bool visEditable = VisibilityIsEditable();
    g_vis.editable = visEditable;
    HC::Repaint(g_vis);

    // The hint differs by mode: on AUTOMATIC the master hands the joiner the identity the proof is
    // bound to; on DIRECT the friend types the host's address and the password into Direct Connect.
    // Written on change only (it depends on g_connMode alone, and this runs on every hover), each
    // hint behind its own null check so one null widget cannot blank the other.
    if ((g_pwHint || g_visHint) && g_hintFor != g_connMode) {
        g_hintFor = g_connMode;
        const bool brokered = (g_connMode == 0);
        if (g_pwHint) SetText(g_pwHint,
                brokered
                    ? std::wstring(L"Anyone with this can join. Give it out the way you "
                                   L"would give out an invite link.")
                    // The DIRECT wording names both values a friend types into Direct Connect.
                    : std::wstring(L"Anyone with this can join. On this connection type "
                                   L"give your friends your address and this password -- "
                                   L"they type both into Direct Connect."),
                brokered ? kDim : kAmber);
        // The same edge. It says why the rows are fixed where they are fixed, and stays quiet on
        // DIRECT: a hint under a usable control is noise.
        if (g_visHint) {
            SetText(g_visHint,
                    g_connMode == 0
                        // Names the control that answers the intent: on this mode the password one
                        // row below is what makes a game private.
                        ? std::wstring(L"Automatic games are always listed -- the list is how "
                                       L"friends find you. Set a password below to keep "
                                       L"strangers out.")
                        : std::wstring(),
                    g_connMode == 1 ? kDim : kAmber);
        }
    }
}

// Turning the lock on mints a password at once (an empty box at this moment is how "1234" happens),
// but only when the box is empty, so toggling off and on keeps a value the player typed or already
// gave out.
void SetLocked(bool locked) {
    if (locked == IsLocked()) return;
    g_who.chosen = locked ? 1 : 0;
    if (IsLocked() && TF::Text(g_pwField).empty()) {
        const std::string pw = GeneratePassword();
        if (pw.empty()) {
            // Fail closed: the RNG refused, so the lock stays off; a padlock with no secret is a
            // false promise.
            g_who.chosen = 0;
            SetStatus(L"Could not generate a password on this system -- the lock stays off.",
                      kBad);
            RepaintChoices();
            return;
        }
        TF::SetText(g_pwField, pw);
    }
    g_fitDirty = true;   // the password block appears/collapses -> the footer moves
    if (!IsLocked()) TF::Blur(g_pwField);   // a collapsed field must not keep the keyboard
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
        // Fail closed and retry; the browser owns the loud alarm for a missing donor, and a second
        // dialog would only stack.
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

    // ---- what step one decided ----
    // The name is editable here (a label on the lobby); the world and the connection mode are not
    // (their consequences reach past this window, so changing them means Back). The autofill stays.
    if (void* recap = SectionBody(col, L"Your session:")) {
        // In a row of its own so kFieldW applies (a VerticalBox slot would stretch the field; see
        // the password row).
        void* nameRow = NS::Spawn(L"HorizontalBox", recap);
        if (!nameRow) return false;
        NS::AddVFill(recap, nameRow, 0.f, NS::kFill, NS::kTop);
        g_nameField = TF::Create(nameRow, L"server name", kNameMaxBytes, kFieldW);
        if (!g_nameField) return false;
        g_recapWorld = NS::AddText(recap, L"", 16, kDim, NS::kJustLeft, 0.f);
        g_recapConn  = NS::AddText(recap, L"", 16, kDim, NS::kJustLeft, 0.f);
        U::SetClipping(g_recapWorld, 1);
        U::SetClipping(g_recapConn, 1);
    }

    if (!HC::Build(col, L"WHO MAY JOIN", kWho, g_who)) { ReleaseFields(); return false; }

    // ---- the password block, collapsed until the lock goes on ----
    // One container, so the block appears and disappears whole; the field must not be ticked off
    // screen either, since its hit test is by geometry and a collapsed widget keeps its last rect.
    g_pwBlock = NS::Spawn(L"VerticalBox", col);
    if (!g_pwBlock) return false;
    NS::AddVFill(col, g_pwBlock, 0.f, NS::kFill, NS::kTop);
    NS::AddText(g_pwBlock, L"Password", 16, kAccent, NS::kJustLeft, 0.f);
    // In a row of its own so kFieldW applies: the field is a SizeBox with a width override, a
    // VerticalBox slot fills horizontally (968 px), and a HorizontalBox slot is auto-sized, which
    // lets the override win.
    void* pwRow = NS::Spawn(L"HorizontalBox", g_pwBlock);
    if (!pwRow) return false;
    NS::AddVFill(g_pwBlock, pwRow, 0.f, NS::kFill, NS::kTop);
    // 64, matching the join prompt: the authoring side must not truncate what the joining side
    // accepts.
    g_pwField = TF::Create(pwRow, L"password", 64, kFieldW);
    if (!g_pwField) return false;
    g_pwHint = NS::AddText(g_pwBlock, L"", 15, kDim, NS::kJustLeft, 0.f);
    if (g_pwHint) U::SetAutoWrapText(g_pwHint, true);
    E::SetWidgetVisibility(g_pwBlock, 1);   // Collapsed: the lock starts off

    // ---- who may find it ----
    // Always present, never collapsed: its rows carry the truth for every mode (kVis) and the hint
    // explains the mode where the choice is not the host's. The field is released on this return
    // too: OnMenuTick retries the build once a second forever after the backoff.
    if (!HC::Build(col, L"SERVER LIST", kVis, g_vis)) {
        ReleaseFields(); return false;
    }
    g_visHint = NS::AddText(col, L"", 15, kDim, NS::kJustLeft, 0.f);
    if (g_visHint) U::SetAutoWrapText(g_visHint, true);

    // A Spacer with all the slack, so the footer sits at the bottom of the window; a Fill slot on
    // the footer would make its height the window's leftover, the arithmetic that pushes buttons
    // off the frame.
    if (void* spacer = NS::Spawn(L"Spacer", col)) NS::AddVFill(col, spacer, 1.f, NS::kFill, NS::kFill);

    // Footer: Back at the left, Host at the right, status between (docs/VOTV_UI_STYLE.md). No
    // bordered strip: the game frames content, never a row of buttons.
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        g_backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        g_status  = NS::AddText(footRow, L"", 16, kText, NS::kJustCenter, 1.f);
        g_hostBtn = NS::BuildButton(footRow, backDonor, L"Host", NS::kBtnFontPx);
        // Release on this path too: a false return leaves g_root null, the next tick rebuilds, and
        // TF::Create would mint a second Field with the first stranded in the module's live list.
        if (!g_backBtn || !g_hostBtn) { ReleaseFields(); return false; }
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft,  NS::kCenter);
        NS::SetHSlot(NS::SlotOf(g_hostBtn), 0.f, NS::kRight, NS::kCenter);
        if (void* s = NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    g_root = shell.root;
    g_box  = shell.box;   // the FRAME, for the fit probe -- root is full-screen

    // Attach at birth, not at first Show(): an unattached widget tree is GC food (it renders until
    // the next collection, then AddChild on the dead object returns null). Not AddToRoot: a
    // switcher child is reachable from the menu, the reference wanted.
    void* slot = U::AddChild(switcher, g_root);
    g_ourIndex = U::IndexOfChild(switcher, g_root);
    if (g_ourIndex < 0) {
        UE_LOGE("host_session_settings: built the session window but could NOT place it in "
                "the menu switcher (AddChild slot=%p, GetChildIndex=-1) -- it cannot be shown "
                "this menu", slot);
        // Release, not Destroy: the tree just built is orphaned, so RemoveChild into it is a call
        // against whatever reuses the slot; clearing g_root alone would leak the heap Field on
        // every rebuild.
        ReleaseFields();
        g_root = nullptr;
        return false;
    }
    UE_LOGI("host_session_settings: screen built (root=%p index=%d) after %d attempt(s)",
            g_root, g_ourIndex, g_buildAttempts + 1);
    return true;
}

// ============================ actions ==================================================

void Hide(const char* why);

// The one host action in the tree; no hosting rule is authored here (session_manager owns them).
void DoHost() {
    g_sawHostAttempt = true;
    const std::string pw = IsLocked() ? TF::Text(g_pwField) : std::string();
    if (IsLocked() && pw.empty()) {
        // A lock with no secret is refused, not announced: the player emptied the box, so they type
        // one or choose "Anyone can join".
        SetStatus(L"Type a password, or choose \"Anyone can join\".", kBad);
        return;
    }

    // Persisted before the attempt, so the next session opens with the lock as the host left it
    // whether this start succeeds or not. The password is written only when there is one: an
    // unconditional write would let an open host erase the remembered password.
    if (IsLocked()) cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_password, pw.c_str());
    cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_locked, IsLocked() ? "1" : "0");

    // The string, not the row just written: a failed write, an env var outranking the file, or
    // trimmed whitespace would read back empty and host open under a lit padlock. The hide gate is
    // stated here, where the value is formed (HostWithSave honours it on DIRECT only). The name
    // comes from the field, never the autofill, sanitised and byte-capped at this boundary (it goes
    // to the master and every browser); a blank falls back to the autofill.
    std::string serverName = coop::text::CapUtf8Bytes(
        coop::text::SanitizeUtf8(TF::Text(g_nameField).data(), TF::Text(g_nameField).size()),
        static_cast<size_t>(kNameMaxBytes));
    while (!serverName.empty() && (serverName.back() == ' ' || serverName.back() == '\t'))
        serverName.pop_back();
    serverName.erase(0, serverName.find_first_not_of(" \t"));
    if (serverName.empty()) serverName = g_name;

    const bool hideFromBrowser = VisibilityIsEditable() && !IsListed();
    const coop::session::HostMode mode{
        g_connMode == 1 ? coop::session::Reachability::Direct
                        : coop::session::Reachability::Brokered,
        !hideFromBrowser};
    const bool accepted = sm::HostWithSave(g_choice, serverName, IsLocked(), pw, /*playersMax=*/4,
                                           mode);
    // The password is never logged: a log line is the easiest place for it to end up in a
    // screenshot.
    UE_LOGI("host_session_settings: HOST %s -- world=%s conn=%d listed=%d locked=%d name='%s'",
            accepted ? "accepted" : "REFUSED (another action in flight)",
            g_choice.newGame ? "<new game>" : g_choice.slot.c_str(), g_connMode,
            // !hideFromBrowser is the listing: AUTOMATIC never hides, DIRECT hides only by this
            // flag.
            !hideFromBrowser ? 1 : 0,
            IsLocked() ? 1 : 0, serverName.c_str());
    if (!accepted) {
        // The window stays open on a refusal: a screen that closes on failure loses the reason with
        // it.
        SetStatus(L"Busy -- another host or join is already starting.", kBad);
        return;
    }
    SetStatus(L"Starting...", kText);
}

// Back returns to step one, not the main menu. Restoring the switcher index alone is not enough:
// the hosting window reconciled itself closed when we took the switcher, and Open() is the one call
// that puts its index and its flag back in agreement.
void BackToHostWindow() {
    Hide("BACK");
    HW::Open();
}

void PollChrome() {
    if (!ui::input_focus::IsOurWindowForeground()) return;

    // ESC. A focused field owns it first, and this poll reads the physical key, so it asks the
    // field at the release edge (below) whether the Escape was its own; the field blurs on
    // WM_KEYDOWN, so a focus test here is always already false.
    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    // Primed only once the key is up, or a window opening under a held Escape would close on the
    // release.
    if (!g_escPrimed) { g_escPrimed = !esc; g_prevEsc = esc; }
    const bool escEdge = g_prevEsc && !esc;   // RELEASE edge
    g_prevEsc = esc;
    if (escEdge) {
        // Consumed at the edge, not every tick: the field sets the latch on WM_KEYDOWN and this
        // poll takes the release dozens of ticks later, so draining it every tick would lose the
        // one true reading. Asked of every field: Escape leaving a field must not also leave the
        // window.
        if (TF::ConsumeEscape(g_nameField)) return;
        if (TF::ConsumeEscape(g_pwField)) return;
        BackToHostWindow();
        return;
    }

    // Enter in a field hosts: the key that ends typing is the one that acts.
    if (TF::ConsumeSubmit(g_nameField)) { DoHost(); return; }
    if (IsLocked() && TF::ConsumeSubmit(g_pwField)) { DoHost(); return; }

    const bool lmb = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = lmb; return; }
    const bool released = g_prevLmb && !lmb;
    g_prevLmb = lmb;
    if (!released) return;

    // Two mechanisms for two kinds of widget (native_screen.h): a real UButton answers IsHovered; a
    // hand-built UImage must come through geometry.
    if (g_backBtn && E::WidgetIsHovered(g_backBtn)) { BackToHostWindow(); return; }
    if (g_hostBtn && E::WidgetIsHovered(g_hostBtn)) { DoHost(); return; }
    // The cursor is resolved once for both selectors; a CursorOverWidget per row is an uncached
    // GUObjectArray walk each.
    long cx = 0, cy = 0;
    if (!NS::CursorInWidgetSpace(cx, cy)) return;   // fail-closed, as the per-widget call was
    int picked = -1;
    if (HC::HandleClick(g_who, cx, cy, picked)) { SetLocked(picked == 1); return; }
    // The visibility rows answer only on DIRECT: HandleClick refuses a non-editable selector, and
    // the dimmed rows plus the hint make the silent no-op honest.
    g_vis.editable = VisibilityIsEditable();
    if (HC::HandleClick(g_vis, cx, cy, picked)) {
        const bool want = (picked == 0);
        if (want != IsListed()) { g_vis.chosen = want ? 0 : 1; RepaintChoices(); }
        return;
    }
}

void UpdateHover() {
    // Gated on the pointer: each CursorOverWidget is a full GUObjectArray walk (FindObjectByClass,
    // no cache) plus four dispatches and an allocation, and two rows at ~117 Hz is the per-frame
    // scan this project forbids. HoverTracker maps one panel's children by index and does not fit
    // two named widgets in a mixed column; its rule (re-evaluate on motion, one settling pass
    // after) is reproduced here.
    static long sLastX = -1, sLastY = -1;
    static bool sSettlePending = false;
    POINT p{};
    if (!::GetCursorPos(&p)) return;
    const bool moved = (p.x != sLastX || p.y != sLastY);
    if (!moved && !sSettlePending) return;
    sLastX = p.x; sLastY = p.y;
    sSettlePending = moved;   // one more pass after motion stops, then quiet

    const int prev = g_who.hover;
    const int prevVis = g_vis.hover;
    g_who.hover = -1;
    g_vis.hover = -1;
    // No g_fitDirty here: a hover reset is not a layout event. The rows are hand-built UImages, on
    // which IsHovered() reads 0, so geometry is the only mechanism; the cursor is converted once
    // for the sweep because each CursorOverWidget redoes the uncached GUObjectArray walk.
    long hx = 0, hy = 0;
    if (!NS::CursorInWidgetSpace(hx, hy)) return;   // fail-closed, as the per-widget call was
    // One call, not a loop: HoverAt sweeps both rows itself.
    g_who.hover = HC::HoverAt(g_who, hx, hy);
    // Only probed where hover means something: on a mode that fixes the answer the rows are dimmed
    // and unclickable.
    g_vis.editable = VisibilityIsEditable();
    if (g_who.hover < 0) g_vis.hover = HC::HoverAt(g_vis, hx, hy);
    if (g_who.hover != prev || g_vis.hover != prevVis) RepaintChoices();
}

// ============================ lifecycle ================================================

// What must be true the moment this screen becomes live, from Show() and from the reconcile that
// revives it. The browser closes on the ESC press edge and this window on the release, so one key
// can close the browser and revive this window, and a stale g_escPrimed would let the same release
// close it too. The content resets stay in Show(): a revive must not wipe a half-typed password or
// the host-failure line.
void BecameLive() {
    g_escPrimed = false;
    g_lmbPrimed = false;
}

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    g_priorIndex = NS::SafePriorIndex(U::SwitcherIndex(g_switcher), g_ourIndex, g_priorIndex);
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    g_who.hover = -1;
    g_vis.hover = -1;
    g_fitDirty  = true;   // a fresh showing lays out again
    // Re-derived only when the mode moved: a Back to change the connection type resets a
    // listed/hidden choice made under other rules, a Back for any other reason keeps a chosen
    // "Hidden". The reset fails open, and what it opens is an announce with the host's address to a
    // player who believes they chose Hidden.
    if (g_visModeWhenChosen != g_connMode) {
        g_vis.chosen = kListedByDefault ? 0 : 1;
        g_visModeWhenChosen = g_connMode;
    }
    BecameLive();
    g_lastStatus.clear();
    g_sawHostAttempt = false;
    g_lastHostStatus.clear();
    // Reset on every open: BuildScreen's failure paths clear g_root without clearing this, and a
    // rebuild would mint a fresh g_pwHint while the cache still matched g_connMode, leaving the
    // hint blank.
    g_hintFor = -2;
    SetStatus(L"", kText);

    // The autofill, written on every opening, so Back-and-return shows the derived "<nick>'s game"
    // rather than whatever was half-typed before leaving.
    TF::SetText(g_nameField, g_name);
    SetText(g_recapWorld,
            g_choice.newGame ? std::wstring(L"World: a new game")
                             : L"World: " + Widen(g_choice.slot),
            kDim);
    SetText(g_recapConn,
            // Two arms, because there are two modes.
            std::wstring(L"Connection: ") +
                (g_connMode == 1 ? L"direct (you forward the port)" : L"automatic"),
            kDim);

    // The lock and the password come back the way they were left.
    const std::string saved = cfg::ResolveString(::coop::config_registry::rows::net_lobby_password);
    TF::SetText(g_pwField, saved);
    // Through the mint path rather than assigned, so an ini with locked=1 and an empty password
    // still ends up with a real secret.
    g_who.chosen = 0;
    RepaintChoices();
    if (cfg::ResolveFlag(::coop::config_registry::rows::net_lobby_locked)) SetLocked(true);

    UE_LOGI("host_session_settings: shown (index %d -> %d; locked=%d)",
            g_priorIndex, g_ourIndex, IsLocked() ? 1 : 0);
    // The fit measurement runs on the next tick (g_fitDirty, ReportFit); locked + AUTOMATIC is the
    // tallest cell.
    g_fitLast = -9999.f;   // re-arm the fit probe for this showing (see ReportFit)
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
    // The intent carries the decision; re-deriving it would mean this window reading the save list
    // itself.
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pending.choice   = choice;
        g_pending.name     = serverName;
        g_pending.connMode = connMode;
    }
    g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

bool IsOpen() { return g_shown; }

void* LockRow()    { return g_who.bg[1]; }
void* BackButton() { return g_backBtn; }
bool  Locked()     { return IsLocked(); }

int PasswordLength() {
    // The field, not the ini row: the self-check proves that clicking the lock put a value in front
    // of the player this session, and a row read would answer yes on a rig whose ini already
    // carried one.
    return static_cast<int>(TF::Text(g_pwField).size());
}

int GeneratedPasswordLength() { return kPwLen; }

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        // Release, not Destroy: the widgets died with the menu instance, so the field unhooks its
        // focus and frees its handle without dispatching RemoveChild into a dead tree.
        ReleaseFields();
        g_root = nullptr; g_box = nullptr; g_scrim = nullptr; g_status = nullptr;
        g_backBtn = nullptr; g_hostBtn = nullptr;
        g_pwBlock = nullptr; g_pwHint = nullptr;
        g_recapWorld = g_recapConn = nullptr;
        HC::ClearWidgets(g_who);
        // The visibility rows die with the same menu instance; a widget missing from this block is
        // a dangling pointer RepaintChoices would dispatch into on the next hover. Anything added
        // to the screen owes a line here.
        HC::ClearWidgets(g_vis);
        g_visHint = nullptr;
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0;
        g_lastStatus.clear();   // the widget it cached is gone with the menu
        g_haveStatus = false;
        g_hintFor = -2;
    }

    if (!g_root) {
        // Backed off once hopeless: each attempt is a SwitcherChild walk (ChildCount plus a
        // ClassNameOf per child) and a donor lookup, at ~117 ticks a second, forever, on exactly
        // the path a version migration lands on.
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
            // Step one closes first and synchronously: both screens record the index they replace,
            // so opening on top of a live sibling would make its index ours to restore, and its
            // Back would walk back into us (host_window_native::CloseNow).
            HW::CloseNow();
            Show();
        } else {
            UE_LOGW("host_session_settings: an open request expired unconsumed after %llu ms "
                    "-- no main-menu tick arrived to show the window",
                    static_cast<unsigned long long>(age));
        }
    }

    // Reconcile against the live index in both directions: a sibling or the game's ESC path can
    // navigate away (we are closed, whoever did it), and the index can come back to ours (we are
    // live again, whoever restored it). Without the second half a caller handing the switcher back
    // by writing the index gets a window that draws with no hover, no clicks and no Back. The index
    // is only ever ours because someone restored it, so the flag can follow the truth.
    const bool indexIsOurs = g_root && g_ourIndex >= 0 &&
                             NS::ActiveIndex() == g_ourIndex;
    if (g_shown && !indexIsOurs) {
        TF::Blur(g_pwField);
        g_shown = false;
        return;
    }
    if (!g_shown) {
        if (!indexIsOurs) return;
        g_shown = true;
        BecameLive();
        UE_LOGI("host_session_settings: live again (the switcher index returned to ours)");
    }

    // Only while the block is on screen: Tick takes focus on a press inside the field's rect by
    // geometry, and a collapsed widget keeps its last rect, so a hidden field would steal the
    // keyboard from a click on the row above.
    TF::Tick(g_nameField);   // never collapsed, so unlike the password it is always live
    if (IsLocked()) TF::Tick(g_pwField);
    ReportFit();   // no-op unless the layout moved -- see g_fitDirty
    UpdateHover();
    PollChrome();

    // The host's failure is read here: HostWithSave fails on worker threads long after returning
    // true, its reason goes to session_manager::HostStatus(), and step one's poll sits on a window
    // CloseNow() already hid, so without this the player waits on "Starting..." forever. Gated on
    // change (a mutex, a copy and a UTF-16 conversion) and to this opening's own attempt, so an
    // older line never paints over Show()'s blank.
    if (g_status && g_sawHostAttempt) {
        std::string cur = sm::HostStatus();
        if (cur != g_lastHostStatus) {
            g_lastHostStatus.swap(cur);
            if (!g_lastHostStatus.empty()) SetStatus(Widen(g_lastHostStatus), kText);
        }
    }
}

}  // namespace ui::host_session_settings
