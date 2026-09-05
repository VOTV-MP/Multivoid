// ui/host_session_settings.cpp -- see ui/host_session_settings.h for why this is its own window
// rather than three more rows on the hosting one.
//
// Built on ui/native_screen's kit and shaped after its two siblings in the same switcher. Where a
// construction fact is measured it lives in the kit's header and is not repeated.

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

// ---- layout --------------------------------------------------------------------------
// The same width as step one, on purpose: the player advances from one window to the next, and
// a frame that changes size under them reads as a different program rather than as the next
// page. Shorter, because there is less to decide here.
constexpr float kWindowW  = 980.f;
// The window keeps one height across both lock states rather than shrinking when the password
// block collapses: a window that resizes as you click inside it is worse to use than one with
// space in it (the browser's details panel makes the same call). It is sized for the locked
// state, whose hint line is the tallest content, and the unlocked state carries the difference
// as slack above the footer. The visibility selector is sized in rather than collapsed per
// connection mode: the mode is chosen in the previous window and cannot change while this one
// is open, so a mode-dependent height would be two layouts to keep true for a hole the spacer
// already absorbs. The number carries about 57 px of margin over the measured need, because
// the tallest cell's hint auto-wraps and its height is a function of the font and the string;
// ReportFit logs an error the moment any cell overflows.
constexpr float kWindowH  = 690.f;
constexpr float kRowH     = 56.f;
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
constexpr float kFieldW   = 420.f;
// The server name's bound, in bytes, because that is what the field counts and what travels.
// The derived default is "<nick>'s game" and a nick is capped at 20 codepoints, so a typed name
// gets comfortably more room than the autofill it replaces while staying a name rather than a
// paragraph: the browser draws it in one row, and the master stores it. 96 bytes is 24
// codepoints at the 4-byte worst case and about 96 in plain Latin.
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

// ---- the two answers to the one question ----------------------------------------------
// The wording is the product surface and is fixed once here. Each line says what the choice
// costs, because "Locked / Unlocked" alone asks the player to guess which is which. Typed as
// the selector's own `Answer`, not a look-alike struct: two identical shapes that must stay in
// step are one shape with two names, and the compiler cannot say when they drift.
constexpr HC::Answer kWho[2] = {
    {L"Anyone can join",
     L"Your session is open to everyone who finds it."},
    {L"Password required",
     L"Only players you give the password to."},
};

// ---- and the second question: can anyone find it -----------------------------------------
// The same control as the ~ menu's "Show in server browser", at the point where the decision is
// made: a host decides listing at creation, before the announce with their address goes out.
// Positive polarity, matching the ~ menu; two surfaces for one decision that read opposite ways
// is how a player ends up certain they set it and wrong about which way.
// It is not editable in every mode, and the rows say so rather than disappearing:
//   AUTOMATIC  always listed. The master is a relay game's only rendezvous, so a hidden one is
//              unjoinable by anyone; the ~ menu is where un-listing becomes legitimate, once
//              friends are already in.
//   DIRECT     the real choice, and the reason this control exists. "Hidden" here means what
//              a LAN-only mode would mean, minus an accept filter doing the router's job
//              (coop/session/host_mode.h).
// So the selector always states the truth about what will happen, and only DIRECT lets you
// move it. That is why these are rows and not a checkbox: a checkbox cannot be truthful and
// unavailable at the same time.
constexpr HC::Answer kVis[2] = {
    {L"Show in server browser",
     L"Anyone can find your game in the list."},
    {L"Hidden",
     L"Only friends you give your address to."},
};

// ---- the generated password -----------------------------------------------------------
// The alphabet has no I, l, 1, O or 0. The value is read aloud over voice chat and typed back by
// hand, so the characters that cost a retry are the ones that look alike in a UI font:
// twenty-six letters minus I and O, ten digits minus 0 and 1, and lower case dropped whole
// because "was that a capital?" is the same retry.
constexpr char kPwAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int  kPwAlphabetN  = 32;   // sizeof - 1, stated so the modulo claim below is checkable
// Six characters, and the number decides what the rest of the system is for, so it is written
// down here rather than left as a length. 6 x log2(32) = 30 bits. Against PBKDF2-HMAC-SHA256 at
// `lobby_password.h`'s 200 000 iterations that is ~2^30 x 4e5 hash operations, about six
// GPU-hours to exhaust, so a captured proof is not worthless by arithmetic alone.
// That is safe only because of something else: a proof is sent only to a host the joiner has
// bound to an identity it was given in advance, or to a destination the player typed themselves
// (`peer_admission.cpp`), so a stranger who merely answers gets no tag to grind. Two
// consequences while this constant is 6: the binding gate may not be relaxed "because the
// generated secret is strong" (at 30 bits it is not), and the typed-address lane's honest price
// is the typo case, a mistyped address answered by an unrelated host, which then learns a
// six-character password to a lobby it cannot find.
constexpr int  kPwLen        = 6;

static_assert(sizeof(kPwAlphabet) - 1 == kPwAlphabetN, "the alphabet and its size must agree");
// 256 % 32 == 0, so `byte % 32` is exactly uniform and rejection sampling is not merely
// skipped, it is unnecessary. This is the one arithmetic that makes the modulo safe; a
// 33-character alphabet would silently bias the first character class.
static_assert(256 % kPwAlphabetN == 0, "modulo would bias the alphabet");

// Empty on failure, and the caller must treat that as a refusal, never as a reason to reach for
// `rand()`: a predictable lobby password is worse than an open lobby, because the padlock tells
// the host they are protected.
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

// ---- state (game thread only unless marked) ------------------------------------------
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
// The questions, each a `host_session_choices::Selector`. `chosen` carries the answer, so there
// are no parallel booleans to keep in step with the widgets.
HC::Selector g_who;   // 0 = anyone may join, 1 = a password is required
HC::Selector g_vis;   // 0 = listed in the server browser, 1 = hidden

// Read through these, never off `chosen` directly: the index-to-meaning mapping belongs in one
// place.
bool IsLocked() { return g_who.chosen == 1; }
bool IsListed() { return g_vis.chosen == 0; }
void* g_visHint     = nullptr;   // why the choice is fixed, on the modes where it is

TF::Field* g_pwField   = nullptr;
TF::Field* g_nameField = nullptr;   // the server name, editable here (see the recap block)

// Every early return from the build past the first `TF::Create` comes through here, so a
// release pair cannot be forgotten on one path: this makes the count one.
void ReleaseFields() {
    TF::Release(g_nameField); g_nameField = nullptr;
    TF::Release(g_pwField);   g_pwField   = nullptr;
}

int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;
int     g_buildAttempts = 0;
bool    g_toldTheUser   = false;

// What step one decided. Carried, never re-derived: exactly one place in the tree reads the save
// list and the connection rows, and it is the window the player just used (see the header).
sm::SaveChoice g_choice;
std::string    g_name;
int            g_connMode = 0;


// The footer-inside-the-frame measurement (see ReportFit): the last reported overflow, so the
// line is logged on change rather than per tick. The value moves exactly twice per showing
// (layout settling, then the lock click), and both are the states worth seeing.
float g_fitLast = -9999.f;

// This window is fixed-height with every child auto-sized and only a Spacer to absorb slack, so
// content taller than `kWindowH` pushes the footer past the bottom edge, and nothing clips: the
// buttons render outside the ring. This screen has no shrinkable child, so its only defence is
// being tall enough. That is a number, so it is measured instead of eyeballed.
// On the tick, not in `Show()`: on the frame the switcher index changes, Slate has not laid the
// subtree out yet and every rect reads 0x0, which would read as "no overflow".
// Edge-driven, not just edge-logged: measuring every tick is six ProcessEvent dispatches and
// eight heap allocations at the menu's ~117 Hz for a value that can only move when the layout
// does. The rects change on the lock toggle and on a fresh showing, and `g_fitDirty` is armed at
// exactly those two. A rebuild while the window is shown does not arm it: a rebuild clears
// `g_root`, which closes the window on the same tick.
bool g_fitDirty = true;

// Which connection mode the visibility answer was chosen under; -1 = never chosen, so the first
// Show() always derives. See Show() for why this exists rather than a plain reset.
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
    // Cleared only here, after both rects read non-degenerate: on the frame the switcher index
    // changes Slate has not laid the subtree out and every rect is 0x0, so clearing on entry would
    // consume the dirty flag on the one tick that cannot answer.
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

// Listed by default in both modes: AUTOMATIC must be listed to be joinable at all, and DIRECT is
// the one where the host chooses (defaulting to listed, the behaviour every DIRECT host has
// always had). There is no third mode: a LAN-only mode that forced `listed=false` and refused
// non-private remotes is identical to DIRECT + Hidden, a value this selector already expresses,
// so the mode that could not choose became the choice itself. See coop/session/host_mode.h.

// Can this connection mode's visibility be moved at all? DIRECT only; see kVis.
bool VisibilityIsEditable() { return g_connMode == 1; }

// Both modes start listed (AUTOMATIC because a hidden one is unjoinable, DIRECT because that is
// what every DIRECT host has), so a per-mode predicate could only ever return true. A predicate
// that cannot vary is not a predicate; the default is stated where it is used.
constexpr bool kListedByDefault = true;

// The status last written, both the string and the colour, because this line changes colour on
// a refusal and a cache that remembers only the text would suppress the repaint that turns it
// red. Module-level and cleared with the widget on the menu-instance edge: a function-local
// static would outlive the UTextBlock it caches for and leave the line permanently blank on the
// second visit.
std::wstring g_lastStatus;
FLinearColor g_lastStatusColor{};
bool         g_haveStatus = false;
// The same discipline for the hint's cache: module-level, cleared with the widget on the
// menu-instance edge, for the reason recorded three lines above.
int          g_hintFor = -2;
// Has this opening of the window attempted a host? The status string is global and outlives
// the action, so without this the screen would show the last one on open.
bool         g_sawHostAttempt = false;
std::string  g_lastHostStatus;

bool g_prevLmb = false, g_lmbPrimed = false;
bool g_prevEsc = false, g_escPrimed = false;

// Cross-thread open/close intent, the shape both siblings use, plus a payload, which they do not
// have. The payload is why there is a mutex: the siblings' intents are bare timestamps, so an
// atomic says everything there is to say; this one carries a `SaveChoice` and two strings, and
// an atomic timestamp published beside a plain struct orders nothing: the game thread could read
// a half-written `std::string`, and the failure would be a crash in the recap line, nowhere near
// the writer. Today the only caller is on the game thread, which is exactly the condition under
// which such a race stays invisible until someone adds a second one.
struct PendingOpen {
    sm::SaveChoice choice;
    std::string    name;
    int            connMode = 0;
};
PendingOpen           g_pending;
std::mutex            g_pendingMu;
std::atomic<uint64_t> g_wantOpenMs{0};
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

// Edge-gated, like every status line in these screens: `SetWidgetText` is two dispatches plus
// an FText, and this one changes only when a host attempt finishes or a refusal fires.
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

// The selectable rows live in `ui/host_session_choices`, one body serving the two questions here
// and the connection-mode one in the hosting window. A row is SizeBox -> Overlay -> [ Image (the
// hit target and the selection fill), HorizontalBox of text ], with no UButton, for the reason
// the browser's rows record: a bare UImage is what we can paint, and a UButton would add a press
// visual we would then have to suppress.

// A titled framed section, the shape `server_browser_panels::SectionBody` uses. Kept local rather
// than lifted into the kit: this is the second caller, and the kit's rule is no new shared
// framework before three working cases.
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
    // Both style channels live in `host_session_choices::Repaint`; see its comment for why
    // selection and hover are independent.
    HC::Repaint(g_who);
    // The password block is collapsed, not hidden. Collapsed (1) takes no space, so the window
    // closes up around it; Hidden (2) would keep a field-sized hole. This is the one place in these
    // screens where the difference is wanted the other way round from the row-alignment case.
    if (g_pwBlock) E::SetWidgetVisibility(g_pwBlock, IsLocked() ? 0 : 1);

    // The visibility rows, the same two channels, but dimmed whole when the mode decides it. A row
    // the player cannot move must not look like one they can: it keeps the selection fill (it is
    // still stating what will happen) and loses the white, so the section reads as information
    // rather than as a control that ignores clicks.
    const bool visEditable = VisibilityIsEditable();
    g_vis.editable = visEditable;
    HC::Repaint(g_vis);

    // The hint says something different on AUTOMATIC and DIRECT. A password proof is only sent to a
    // host the joiner has bound to an advertised identity, or to an address the player typed
    // themselves (`coop/net/lobby_password.h`, `peer_admission.cpp`). On AUTOMATIC the master hands
    // the joiner the identity and it is invisible to everyone; on DIRECT nothing in the middle
    // hands one over, so the friend types the host's address and the password into Direct Connect,
    // and the hint says exactly that.
    // Written on change only: it depends solely on `g_connMode`, which cannot move while this
    // window is open, and `RepaintChoices` runs on every hover transition, so an ungated write is
    // two dispatches and a ~230-character wstring per mouse sweep, even while the block is
    // collapsed. Either hint arms the edge, and each is written behind its own null check: gating
    // both on `g_pwHint` would let one null widget silently blank the other, and `BuildScreen`
    // succeeds with either null.
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
        // The same edge, so the visibility hint costs nothing extra per hover sweep. It says why
        // the rows are fixed on the mode that fixes them, and stays quiet on DIRECT where they are
        // not: a hint under a control the player can just use is noise.
        if (g_visHint) {
            SetText(g_visHint,
                    g_connMode == 0
                        // It names the control that answers the intent: a player pressing "Hidden"
                        // wants a private game, and on this mode the password one row below is what
                        // gives them one; an unlisted brokered lobby would just be unjoinable.
                        ? std::wstring(L"Automatic games are always listed -- the list is how "
                                       L"friends find you. Set a password below to keep "
                                       L"strangers out.")
                        : std::wstring(),
                    g_connMode == 1 ? kDim : kAmber);
        }
    }
}

// Turning the lock on mints a password immediately. A lock that opened an empty box would make
// the player invent a password at the worst possible moment, the one where they are in a hurry
// to start a game, and that is how "1234" happens. It mints only when the box is empty, so
// toggling the lock off and on again does not throw away a value the player typed or already
// told a friend.
void SetLocked(bool locked) {
    if (locked == IsLocked()) return;
    g_who.chosen = locked ? 1 : 0;
    if (IsLocked() && TF::Text(g_pwField).empty()) {
        const std::string pw = GeneratePassword();
        if (pw.empty()) {
            // Fail closed: the RNG refused, so the lock does not go on. A padlock with no secret
            // behind it is a false promise.
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
        // Fail closed and retry. The browser owns the loud player-facing alarm for a missing donor;
        // this screen is two doors behind it, so a second dialog would only stack.
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
    // The name is editable here; the world and the connection mode are not. Those two are step-one
    // decisions whose consequences reach past this window (changing either means going Back and
    // re-choosing), but the name is a label on the lobby, and a player who wants to rename their
    // game should not have to leave the window they are about to press Host in. The autofill stays:
    // an empty box at this moment would make them invent a name to get past it.
    if (void* recap = SectionBody(col, L"Your session:")) {
        // In a row of its own so `kFieldW` applies: a VerticalBox slot fills horizontally and would
        // stretch the field to the full column, the trap the password row documents below.
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

    // ---- the password block, collapsed until the lock goes on ---------------------------
    // One container, so the whole block appears and disappears together. The field itself cannot be
    // toggled (`native_text_field` deliberately exposes no widget), and it must not be ticked while
    // it is off screen either, because its hit test is by geometry and a collapsed widget keeps the
    // rect it last painted with.
    g_pwBlock = NS::Spawn(L"VerticalBox", col);
    if (!g_pwBlock) return false;
    NS::AddVFill(col, g_pwBlock, 0.f, NS::kFill, NS::kTop);
    NS::AddText(g_pwBlock, L"Password", 16, kAccent, NS::kJustLeft, 0.f);
    // In a row of its own, so `kFieldW` applies. The field is a SizeBox with a width override, and
    // a VerticalBox slot fills horizontally by default, so parenting it straight to the column
    // would stretch it to the full 968 px and the constant would do nothing. A HorizontalBox slot
    // is auto-sized, which is what lets the override win.
    void* pwRow = NS::Spawn(L"HorizontalBox", g_pwBlock);
    if (!pwRow) return false;
    NS::AddVFill(g_pwBlock, pwRow, 0.f, NS::kFill, NS::kTop);
    // 64, matching the join prompt: the authoring side must not truncate what the joining side
    // accepts, or a host who typed a 60-character passphrase hosts with part of it and their
    // friends' correct password never matches.
    g_pwField = TF::Create(pwRow, L"password", 64, kFieldW);
    if (!g_pwField) return false;
    g_pwHint = NS::AddText(g_pwBlock, L"", 15, kDim, NS::kJustLeft, 0.f);
    if (g_pwHint) U::SetAutoWrapText(g_pwHint, true);
    E::SetWidgetVisibility(g_pwBlock, 1);   // Collapsed: the lock starts off

    // ---- who may find it ----------------------------------------------------------------
    // Always present, never collapsed: unlike the password block this does not appear and disappear
    // with a click inside the window, and its rows carry the truth for every mode (see kVis). The
    // hint under them explains the mode where the choice is not the host's. The field is released
    // on the way out: this return is after `TF::Create`, so a bare `return false` would leak a
    // `Field` on every retry, and `OnMenuTick` retries once a second forever after the backoff.
    if (!HC::Build(col, L"SERVER LIST", kVis, g_vis)) {
        ReleaseFields(); return false;
    }
    g_visHint = NS::AddText(col, L"", 15, kDim, NS::kJustLeft, 0.f);
    if (g_visHint) U::SetAutoWrapText(g_visHint, true);

    // A Spacer with all the slack, so the footer sits at the bottom of the window rather than
    // floating under the last control. The same trick the input windows use, and it costs one
    // widget; the alternative, a Fill slot on the footer, makes the footer's own height the
    // window's leftover, exactly the arithmetic that pushes buttons off the frame.
    if (void* spacer = NS::Spawn(L"Spacer", col)) NS::AddVFill(col, spacer, 1.f, NS::kFill, NS::kFill);

    // Footer: Back at the left, Host at the right, status between. Bottom-left is the way out and
    // bottom-right is the commit, in every native game window that has both
    // (docs/VOTV_UI_STYLE.md). No bordered strip around them: the game frames content, never a row
    // of buttons, and a strip reads as a second window edge.
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        g_backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        g_status  = NS::AddText(footRow, L"", 16, kText, NS::kJustCenter, 1.f);
        g_hostBtn = NS::BuildButton(footRow, backDonor, L"Host", NS::kBtnFontPx);
        // Release on this path too. Returning false here leaves `g_root` null, so the next tick
        // rebuilds the whole screen and `TF::Create` mints a second Field, the first stranded in
        // the module's live list forever.
        if (!g_backBtn || !g_hostBtn) { ReleaseFields(); return false; }
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft,  NS::kCenter);
        NS::SetHSlot(NS::SlotOf(g_hostBtn), 0.f, NS::kRight, NS::kCenter);
        if (void* s = NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    g_root = shell.root;
    g_box  = shell.box;   // the FRAME, for the fit probe -- root is full-screen

    // Attach at birth, not at first Show(). An unattached widget tree is GC food: it renders until
    // the next collection and then vanishes, and `AddChild` on the dead object returns null. Both
    // siblings attach here for the same reason. `AddToRoot` is the wrong tool: a switcher child is
    // reachable from the menu, which is the reference wanted.
    void* slot = U::AddChild(switcher, g_root);
    g_ourIndex = U::IndexOfChild(switcher, g_root);
    if (g_ourIndex < 0) {
        UE_LOGE("host_session_settings: built the session window but could NOT place it in "
                "the menu switcher (AddChild slot=%p, GetChildIndex=-1) -- it cannot be shown "
                "this menu", slot);
        // And the field goes with it. Release, not Destroy: the tree just built is orphaned, so
        // dispatching RemoveChild into it is a call against whatever reuses the slot. Clearing
        // `g_root` alone would leave the heap Field alive and in the module's live list, and the
        // next tick would rebuild everything and leak another one.
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

// Host: the one host action in the tree, and the only place it is called from. No hosting rule
// is authored here: if one must change, it changes in `session_manager`, once.
void DoHost() {
    g_sawHostAttempt = true;
    const std::string pw = IsLocked() ? TF::Text(g_pwField) : std::string();
    if (IsLocked() && pw.empty()) {
        // A lock with no secret is a false promise, so it is refused here rather than announced.
        // The player emptied the box themselves; the way out is to type something or to choose
        // "Anyone can join".
        SetStatus(L"Type a password, or choose \"Anyone can join\".", kBad);
        return;
    }

    // Persisted before the attempt, so a host who has told their friends a value keeps it whether
    // or not this particular start succeeds, and the next session opens with the lock the way they
    // left it. The password is written only when there is one: writing `pw` unconditionally would
    // let one "Anyone can join" host erase the remembered password, the opposite of the lock's
    // mint-only-when-empty rule.
    if (IsLocked()) cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_password, pw.c_str());
    cfg::WriteIniValue(::coop::config_registry::rows::net_lobby_locked, IsLocked() ? "1" : "0");

    // The string, not the row just written. The ini is where it is remembered, not the channel by
    // which it reaches the host call: a write that failed, an env var that outranks the file, or
    // edge whitespace the parser trims would all read back empty and silently downgrade this
    // session to open while the padlock stays lit on screen.
    // Hide only where hiding is a choice. `hideFromBrowser` is DIRECT-only in `HostWithSave`
    // (AUTOMATIC ignores it by design), so the mode gate is stated here, where the value is formed.
    // The name the player sees is the name hosted under, read from the field, never from `g_name`
    // (the autofill this window opened with): using that would make the box a decoration that
    // discards what was typed into it. Sanitised and bounded here, because this is the boundary:
    // from this call the string goes to the master and into every other player's browser.
    // `SanitizeUtf8` is the nick path's C0-control denylist, and the byte cap backs off to a
    // character boundary rather than splitting a sequence; the field already bounds input at
    // `kNameMaxBytes`, and capping again does not trust the widget. A blank box falls back rather
    // than refusing: a nameless row in the browser helps nobody.
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
    // The password is never logged. It is the one secret in this window, and a log line is the
    // easiest place in the whole program for it to end up in a screenshot.
    UE_LOGI("host_session_settings: HOST %s -- world=%s conn=%d listed=%d locked=%d name='%s'",
            accepted ? "accepted" : "REFUSED (another action in flight)",
            g_choice.newGame ? "<new game>" : g_choice.slot.c_str(), g_connMode,
            // `!hideFromBrowser` is the listing: AUTOMATIC never hides, and DIRECT hides only by
            // this flag.
            !hideFromBrowser ? 1 : 0,
            IsLocked() ? 1 : 0, serverName.c_str());
    if (!accepted) {
        // The window stays open on a refusal: a screen that closes on failure leaves the reason on
        // the one surface that has just gone away.
        SetStatus(L"Busy -- another host or join is already starting.", kBad);
        return;
    }
    SetStatus(L"Starting...", kText);
}

// Back returns to step one, not to the main menu: this window is only reached from the hosting
// window, and a two-step flow whose Back exits the flow entirely would make the player redo the
// world choice to change one connection mode. Restoring the switcher index is not enough on its
// own: the hosting window tracks its own shown flag and reconciled itself closed when we took
// the switcher, so it would consider itself hidden while its pixels were on screen. Asking it
// to `Open()` is the one call that puts the index and that flag back in agreement, the rule the
// input windows follow with the browser.
void BackToHostWindow() {
    Hide("BACK");
    HW::Open();
}

void PollChrome() {
    if (!ui::input_focus::IsOurWindowForeground()) return;

    // ESC. A focused field owns it first: the field turns Escape into "leave the field", and this
    // poll reads the physical key, so swallowing the message at the WndProc seam would not stop
    // this edge. One press must not both blur and go back. The field is asked whether the Escape
    // was its own at the release edge (below): the field blurs on WM_KEYDOWN and this poll takes
    // the key-up, so a focus test here is always already false.
    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    // Primed only once the key is up. Seeding `g_prevEsc` from a held key still arms the release
    // edge, so a window opening under a held Escape would close itself on the release.
    if (!g_escPrimed) { g_escPrimed = !esc; g_prevEsc = esc; }
    const bool escEdge = g_prevEsc && !esc;   // RELEASE edge
    g_prevEsc = esc;
    if (escEdge) {
        // Consumed at the edge, not every tick. The field sets the latch on WM_KEYDOWN and this
        // poll takes the release edge dozens of ticks later, so draining the latch unconditionally
        // at the top of the poll would throw the one true reading away on the key-down tick, and
        // the release would always see false: one Escape would blur the field and close the window.
        // Asking only at the edge means the latch survives the whole press, which is what "this
        // Escape was the field's" has to mean when press and release are different ticks. Asked of
        // every field, not just the password's: Escape leaving a field must not also leave the
        // window.
        if (TF::ConsumeEscape(g_nameField)) return;
        if (TF::ConsumeEscape(g_pwField)) return;
        BackToHostWindow();
        return;
    }

    // Enter in a field hosts. The key that ends typing is the one that acts, as in every text field
    // a player has used.
    if (TF::ConsumeSubmit(g_nameField)) { DoHost(); return; }
    if (IsLocked() && TF::ConsumeSubmit(g_pwField)) { DoHost(); return; }

    const bool lmb = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = lmb; return; }
    const bool released = g_prevLmb && !lmb;
    g_prevLmb = lmb;
    if (!released) return;

    // Two mechanisms, because there are two kinds of widget (`native_screen.h`): a real UButton
    // answers `IsHovered`; a hand-built UImage does not and must come through geometry.
    if (g_backBtn && E::WidgetIsHovered(g_backBtn)) { BackToHostWindow(); return; }
    if (g_hostBtn && E::WidgetIsHovered(g_hostBtn)) { DoHost(); return; }
    // The cursor is resolved once for both selectors, exactly as the hover sweep does it; a
    // `CursorOverWidget` per row is an uncached GUObjectArray walk each.
    long cx = 0, cy = 0;
    if (!NS::CursorInWidgetSpace(cx, cy)) return;   // fail-closed, as the per-widget call was
    int picked = -1;
    if (HC::HandleClick(g_who, cx, cy, picked)) { SetLocked(picked == 1); return; }
    // The visibility rows answer only on DIRECT: `HandleClick` refuses a non-editable selector
    // itself, so the click is swallowed rather than redirected. The rows are dimmed and the hint
    // says why, so a silent no-op is honest and a status line would be nagging.
    g_vis.editable = VisibilityIsEditable();
    if (HC::HandleClick(g_vis, cx, cy, picked)) {
        const bool want = (picked == 0);
        if (want != IsListed()) { g_vis.chosen = want ? 0 : 1; RepaintChoices(); }
        return;
    }
}

void UpdateHover() {
    // Gated on the pointer, because each `CursorOverWidget` is expensive in a way its name does not
    // suggest: it reaches `GetWorldContext()` and `FindObjectByClass`, a full GUObjectArray walk
    // with no result cache, plus four ProcessEvent dispatches and a heap allocation for the rect;
    // two rows every menu tick at ~117 Hz is the per-frame full-array scan this project forbids.
    // `HoverTracker` is the shared answer for a list and does not fit here: it maps the children of
    // one panel by index, and these are two named widgets in a mixed column. What it supplies that
    // matters, re-evaluate only when the pointer moved and owe one settling pass after it stops, is
    // reproduced here and nothing more.
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
    // No `g_fitDirty` here: this is the per-sweep hover reset, not a layout event, and arming it
    // from the pointer path would re-dirty the probe on every mouse move. These rows sit in the
    // content column, outside any ScrollBox, but they are hand-built UImages, and `IsHovered()`
    // reads 0 on one of those whether or not it is inside a scroll container; geometry is the only
    // mechanism that answers for them. The cursor is resolved once for the whole sweep: each
    // `CursorOverWidget` redoes that conversion, and its expensive half is the uncached
    // `GUObjectArray` walk, so probing four rows through it would cost four walks per tick for a
    // question whose cursor half is the same for all four.
    long hx = 0, hy = 0;
    if (!NS::CursorInWidgetSpace(hx, hy)) return;   // fail-closed, as the per-widget call was
    // One call, not a loop: `HoverAt` sweeps both rows itself.
    g_who.hover = HC::HoverAt(g_who, hx, hy);
    // Only probed where hover means something: where the mode fixes the answer the rows are dimmed
    // and unclickable, so lighting one up would promise a control that is not there.
    g_vis.editable = VisibilityIsEditable();
    if (g_who.hover < 0) g_vis.hover = HC::HoverAt(g_vis, hx, hy);
    if (g_who.hover != prev || g_vis.hover != prevVis) RepaintChoices();
}

// ============================ lifecycle ================================================

// Everything that must be true the moment this screen becomes live, with one owner, because it
// happens two ways: `Show()`, and the reconcile that revives it when the switcher index comes
// back to ours. The revive needs these too: the browser closes on the ESC press edge while this
// window closes on the release, so one keypress can close the browser and revive this window,
// and a stale `g_escPrimed` would let the player's own release close this one as well, one key
// walking two screens back. The hover matters for the same reason: a reopening does not move
// the pointer. The content resets stay in `Show()` on purpose: re-reading the ini or clearing
// the status on a revive would wipe a half-typed password and erase the very host-failure line
// this window exists to display.
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
    // Re-derived only when the mode moved, not on every open. The player may have gone Back and
    // changed the connection type, and a listed/hidden choice made under other rules should not
    // carry across; but a Back for any other reason (fixing the world name) must not reset a chosen
    // "Hidden" to listed. The failure direction is what makes this worth the extra state: such a
    // reset fails open, and what it opens is an announce carrying the host's name, world, lock
    // flag, cap, listen port, identity and the source IP the master resolves, to a player who
    // believes they chose Hidden. The lock choice survives the same round trip through the ini, so
    // both answers on this screen behave alike.
    if (g_visModeWhenChosen != g_connMode) {
        g_vis.chosen = kListedByDefault ? 0 : 1;
        g_visModeWhenChosen = g_connMode;
    }
    BecameLive();
    g_lastStatus.clear();
    g_sawHostAttempt = false;
    g_lastHostStatus.clear();
    // Reset on every open, not only on a menu change: `BuildScreen`'s failure paths clear `g_root`
    // without clearing this, so a rebuild would mint a fresh `g_pwHint` while the cache still
    // matched `g_connMode`, and the hint would stay permanently blank.
    g_hintFor = -2;
    SetStatus(L"", kText);

    // The autofill, written on every opening. `g_name` is step one's derived "<nick>'s game", and
    // re-seeding it here is what makes Back-and-return show the name the player expects rather than
    // whatever they had half-typed before leaving.
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

    // The lock and the password come back the way they were left. A host who set one last week
    // should not have to re-read it off a friend's screen.
    const std::string saved = cfg::ResolveString(::coop::config_registry::rows::net_lobby_password);
    TF::SetText(g_pwField, saved);
    // Forced through the mint path rather than assigned, so an ini that says locked=1 with an empty
    // password still ends up with a real secret instead of a padlock over nothing.
    g_who.chosen = 0;
    RepaintChoices();
    if (cfg::ResolveFlag(::coop::config_registry::rows::net_lobby_locked)) SetLocked(true);

    UE_LOGI("host_session_settings: shown (index %d -> %d; locked=%d)",
            g_priorIndex, g_ourIndex, IsLocked() ? 1 : 0);
    // The footer-inside-the-frame measurement runs on the next tick (see g_fitDirty and ReportFit);
    // the locked + AUTOMATIC cell is the tallest.
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
    // The intent carries the decision. Re-deriving it on the game thread would mean this window
    // reading the save list itself, the duplication the header forbids.
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
    // The field, not the ini row: what the self-check has to prove is that clicking the lock put a
    // value in front of the player this session, and a row read would answer yes on a rig whose ini
    // already carried one from a previous run.
    return static_cast<int>(TF::Text(g_pwField).size());
}

int GeneratedPasswordLength() { return kPwLen; }

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        // Release, not Destroy. The widgets died with the menu instance, so the field must unhook
        // its focus and free its handle without dispatching RemoveChild into a tree that no longer
        // exists.
        ReleaseFields();
        g_root = nullptr; g_box = nullptr; g_scrim = nullptr; g_status = nullptr;
        g_backBtn = nullptr; g_hostBtn = nullptr;
        g_pwBlock = nullptr; g_pwHint = nullptr;
        g_recapWorld = g_recapConn = nullptr;
        HC::ClearWidgets(g_who);
        // The visibility rows die with the same menu instance. Missing from this block is not a
        // leak, it is a dangling pointer: `RepaintChoices` runs on the next hover and would
        // dispatch SetImageTint / SetTextBlockColorDispatch into a destroyed widget tree. Anything
        // added to the screen owes a line here.
        HC::ClearWidgets(g_vis);
        g_visHint = nullptr;
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0;
        g_lastStatus.clear();   // the widget it cached is gone with the menu
        g_haveStatus = false;
        g_hintFor = -2;
    }

    if (!g_root) {
        // Backed off once it is hopeless, because the retry is not free: each attempt costs a
        // `SwitcherChild` walk (a ChildCount plus a ClassNameOf per child, an engine call and a
        // wstring each) plus a donor lookup, at ~117 menu ticks a second, forever, on exactly the
        // path a version migration lands on.
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
            // Step one closes first and synchronously. Both screens are children of one switcher
            // and each records the index it replaces, so opening on top of a live sibling makes
            // that sibling's index the one this window would restore, and its Back would then walk
            // back into us. See `host_window_native::CloseNow`.
            HW::CloseNow();
            Show();
        } else {
            UE_LOGW("host_session_settings: an open request expired unconsumed after %llu ms "
                    "-- no main-menu tick arrived to show the window",
                    static_cast<unsigned long long>(age));
        }
    }

    // Reconcile against the live index rather than asserting ours, in both directions: a sibling
    // screen (or the game's own ESC path) can navigate away, and if it did we were closed, whoever
    // did it; and if the index comes back to ours we are on screen again, whoever put it back. The
    // second half matters as much as the first: a window that can lose the screen by observation
    // but regain it only by being told answers nothing when a caller hands the switcher back by
    // writing the index (the obvious way, and what the server browser does): it draws normally
    // with no hover, no clicks, not even its own Back or ESC. Fixing it here rather than in the
    // caller is what makes it hold: the index is only ever ours because someone restored it (the
    // game never navigates to a child we built), so the flag can follow the truth instead of every
    // present and future caller having to know which window it displaced.
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

    // Only while the block is on screen. `Tick` takes focus when the pointer is pressed inside the
    // field's own rect by geometry, and a collapsed widget keeps the rect it last painted with, so
    // ticking a hidden field would let a click on the row above it steal the keyboard into a box
    // the player cannot see.
    TF::Tick(g_nameField);   // never collapsed, so unlike the password it is always live
    if (IsLocked()) TF::Tick(g_pwField);
    ReportFit();   // no-op unless the layout moved -- see g_fitDirty
    UpdateHover();
    PollChrome();

    // Why the host's own failure message is read here. `HostWithSave` announces, loads a world and
    // starts a session on worker threads, so it can only fail long after it returned true, and its
    // reason goes to `session_manager::HostStatus()`. Step one's poll is on a window `CloseNow()`
    // has already hidden, so this window polls it, or every asynchronous host failure is invisible:
    // the player sits on "Starting..." forever while the reason is written to a string nothing
    // draws. Gated before the string is built: `HostStatus()` is a mutex plus a string copy and
    // `Widen` a full UTF-8 to UTF-16 conversion, so they run only on a change, and `SetStatus`
    // edge-gates the widget write behind that. And only what this opening caused: the status
    // outlives the action that set it, so an unfiltered read would paint the previous action's
    // line over Show()'s deliberate blank before the player had pressed anything.
    if (g_status && g_sawHostAttempt) {
        std::string cur = sm::HostStatus();
        if (cur != g_lastHostStatus) {
            g_lastHostStatus.swap(cur);
            if (!g_lastHostStatus.empty()) SetStatus(Widen(g_lastHostStatus), kText);
        }
    }
}

}  // namespace ui::host_session_settings
