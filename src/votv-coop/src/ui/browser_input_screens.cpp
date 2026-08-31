// ui/browser_input_screens.cpp -- see ui/browser_input_screens.h.

#include "ui/browser_input_screens.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"
#include "ui/input_focus.h"
#include "ui/native_screen.h"
#include "ui/native_text_field.h"
#include "ui/server_browser_native.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace ui::browser_input_screens {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace sm = coop::session_manager;
namespace TF = ui::native_text_field;
namespace SB = ui::server_browser_native;

using ue_wrap::FLinearColor;

// Small: a window that holds one label, one field and two buttons has no business being
// the size of the browser. The Language window it copies is about this shape.
constexpr float kWindowW = 620.f;
constexpr float kWindowH = 210.f;
constexpr float kFieldW  = 560.f;

const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kBad    = NS::Bad();

// ---- one screen's widgets ------------------------------------------------------------
struct Screen {
    void*      root    = nullptr;
    void*      scrim   = nullptr;
    void*      backBtn = nullptr;
    void*      okBtn   = nullptr;
    void*      status  = nullptr;
    TF::Field* field   = nullptr;
    int32_t    index   = -1;
    std::string lastStatus;
};

Screen  g_screen[3];                 // indexed by Kind

// WHICH LOBBY THE PASSWORD PROMPT IS FOR. Captured when CONNECT was pressed, not read
// back from the selection when OK is clicked: the list re-fetches every 5 s and a refresh
// re-sorts it, so the highlighted row after a few seconds of typing may be a different
// server -- and joining the wrong one with the right password is a worse outcome than
// either failure it replaces.
struct PendingJoin {
    std::string lobbyId;
    std::string displayName;
    int         hostProto = 0;
    std::string hostGame;
};
PendingJoin g_pendingJoin;
void*   g_menu     = nullptr;
void*   g_switcher = nullptr;
int     g_open     = -1;             // which Kind is showing, or -1
int32_t g_priorIndex = -1;
int     g_buildAttempts = 0;
bool    g_toldTheUser   = false;   // stop hammering the donor lookup once it is hopeless

bool g_prevLmb = false, g_lmbPrimed = false;
bool g_prevEsc = false, g_escPrimed = false;

std::atomic<int>      g_wantOpen{-1};
std::atomic<bool>     g_wantClose{false};
std::atomic<uint64_t> g_wantAtMs{0};
constexpr uint64_t kIntentTtlMs = 20000;

// THE INVERSE OF `Idx`, and it exists because the two call sites that needed it were
// open-coded ternaries reading `g_open == 0 ? DirectConnect : ChangeName`. Adding a third
// screen would have made both of them silently confirm the WRONG one -- a password typed
// into a box that then set the player's nickname.
Kind KindOf(int idx) {
    return idx == 0 ? Kind::DirectConnect
                    : (idx == 1 ? Kind::ChangeName : Kind::LobbyPassword);
}

int Idx(Kind k) {
    return k == Kind::DirectConnect ? 0 : (k == Kind::ChangeName ? 1 : 2);
}

// What each screen SAYS. Kept in one table so the two cannot drift into different idioms
// for the same window.
struct Spec {
    const wchar_t* title;
    const wchar_t* label;
    const wchar_t* hint;
    const wchar_t* confirm;
    int32_t        maxLen;
};
const Spec kSpec[3] = {
    {L"Multivoid  -  Direct connect", L"Server address", L"host or host:port", L"Connect", 64},
    {L"Multivoid  -  Change name",    L"Your name",      L"your name",         L"OK",      24},
    // The cap is 64 CODEPOINTS and not the generated length: a host may replace the
    // generated value with anything they can say out loud, and a limit sized to what WE
    // mint would silently truncate what THEY chose.
    {L"Multivoid  -  Password",       L"Server password", L"password",         L"Join",    64},
};

void SetStatus(Screen& s, const std::string& utf8, const FLinearColor& col) {
    if (!s.status || utf8 == s.lastStatus) return;
    s.lastStatus = utf8;
    const std::wstring w = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
    E::SetWidgetText(s.status, w.c_str());
    E::SetTextBlockColorDispatch(s.status, col);
}

bool BuildOne(void* switcher, Kind kind, void* backDonor) {
    Screen& s = g_screen[Idx(kind)];
    const Spec& spec = kSpec[Idx(kind)];

    NS::WindowShell shell;
    if (!NS::BuildWindowShell(switcher, kWindowW, kWindowH, spec.title, shell)) return false;
    void* col = shell.column;

    NS::AddText(col, spec.label, 16, kAccent, NS::kJustLeft, 0.f);
    s.field = TF::Create(col, spec.hint, spec.maxLen, kFieldW);
    if (!s.field) return false;

    // The status line sits under the field and starts empty: it exists to say why a
    // confirm did not take, and a window that opens already explaining itself is noise.
    s.status = NS::AddText(col, L"", 16, kText, NS::kJustLeft, 0.f);

    // A SPACER WITH ALL THE SLACK, so the footer sits at the bottom of the window rather
    // than floating under the field. One weighted empty text block is the whole trick and
    // it costs one widget; a Fill slot on the footer would push it to the bottom too, but
    // then the footer's own height would be the window's leftover, which is the slack
    // arithmetic the list height already had to stop depending on.
    if (void* spacer = NS::Spawn(L"Spacer", col)) NS::AddVFill(col, spacer, 1.f, NS::kFill, NS::kFill);

    // FOOTER: Back at the LEFT, the confirm at the RIGHT. That is not symmetry, it is what
    // every native VOTV window does -- bottom-right is the CONFIRM position (style doc
    // section 5, gap S7).
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        s.backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        void* gap  = NS::Spawn(L"Spacer", footRow);
        if (gap) NS::AddHFill(footRow, gap, 1.f, NS::kFill, NS::kFill);
        s.okBtn = NS::BuildButton(footRow, backDonor, spec.confirm, NS::kBtnFontPx);
        // RELEASE HERE TOO -- the next tick rebuilds and mints a second Field, stranding
        // this one in the module's live list forever. The index-failure path below was
        // hardened against exactly this and this one was not.
        if (!s.backBtn || !s.okBtn) { TF::Release(s.field); s.field = nullptr; return false; }
        NS::SetHSlot(NS::SlotOf(s.backBtn), 0.f, NS::kLeft, NS::kCenter);
        NS::SetHSlot(NS::SlotOf(s.okBtn), 0.f, NS::kRight, NS::kCenter);
        NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom);
    }

    s.root = shell.root;
    s.scrim = shell.scrim;
    // THE ADD'S RETURN IS CHECKED. Ignoring it is how `IndexOfChild` returns -1 for a
    // reason the log cannot name -- and the browser's own builder checks it.
    void* slot = U::AddChild(switcher, s.root);
    s.index = U::IndexOfChild(switcher, s.root);
    if (s.index < 0) {
        UE_LOGE("browser_input_screens: built '%ls' but could NOT place it in the menu "
                "switcher (AddChild slot=%p, GetChildIndex=-1) -- it cannot be shown this "
                "menu", spec.title, slot);
        // AND THE FIELD GOES WITH IT. Clearing `s.root` alone left the heap `Field` alive
        // and in `g_live`, so the next tick rebuilt everything and leaked another one --
        // ~117 leaked Fields and ~2,800 UObject spawns per second, forever (post-ship perf
        // audit, 2026-08-31). Release, not Destroy: the tree we just built is orphaned.
        TF::Release(s.field);
        s.field = nullptr;
        s.root = nullptr;
        return false;
    }
    return true;
}

void Hide(const char* why);

// THE CONFIRM. Each screen's one action, and both write their value to the ini so the
// answer survives the session -- which is the whole difference between a text box and a
// setting.
void Confirm(Kind kind) {
    Screen& s = g_screen[Idx(kind)];
    const std::string value = TF::Text(s.field);
    if (kind == Kind::LobbyPassword) {
        if (value.empty()) {
            SetStatus(s, "Type the password this server was locked with.", kBad);
            return;
        }
        // HANDED OVER, NEVER STORED. `SetJoinPassword` holds it for exactly one join
        // attempt and nothing writes it to the ini -- it is a secret the player was
        // lent, and the ini is the file people paste into bug reports.
        sm::SetJoinPassword(value);
        if (!sm::JoinLobby(g_pendingJoin.lobbyId, g_pendingJoin.displayName,
                           g_pendingJoin.hostProto, g_pendingJoin.hostGame)) {
            // The password is dropped on a refusal so it cannot ride along into a
            // different connection the player makes next.
            sm::SetJoinPassword("");
            SetStatus(s, "Could not start that connection -- another action is already "
                         "in flight.", kBad);
            return;
        }
        UE_LOGI("browser_input_screens: join with a password accepted -- join_progress "
                "owns the player from here");
        Hide("joining");
        return;
    }
    if (kind == Kind::ChangeName) {
        if (value.empty()) {
            SetStatus(s, "Type a name first.", kBad);
            return;
        }
        sm::SetNickname(value);
        coop::config::WriteIniValue(::coop::config_registry::rows::net_nick, value.c_str());
        UE_LOGI("browser_input_screens: nickname set from the Change name window");
        Hide("name accepted");
        SB::Open();
        return;
    }

    if (value.empty()) {
        SetStatus(s, "Type an address first -- host or host:port.", kBad);
        return;
    }
    // `ConnectDirect` OWNS the refusal. It parses the address and answers false for a bad
    // one, so this does not re-implement the parse -- a second parser is a second opinion
    // about what a valid address is, and the one that matters is the one that dials.
    // NOT A LOCKED-ROW JOIN, so nothing typed for a previous server may ride along.
    // `TakeJoinPassword` consumes the transient now, but a value set by the prompt and
    // then abandoned (the player pressed Back) would still be sitting there, and
    // `ConnectDirect` reads it. The browser's own unlocked-row path clears for exactly
    // this reason and this door did not (post-ship audit, 2026-08-31).
    sm::SetJoinPassword("");
    if (!sm::ConnectDirect(value)) {
        SetStatus(s, "Could not connect to that address -- check it, or another action is "
                     "already in flight.", kBad);
        return;
    }
    // WRITTEN ONLY AFTER THE ACCEPT GATE PASSED. `browser.lastdirect` means "the last
    // address that was actually tried and accepted", so a typo cannot overwrite a
    // known-good address the player will want back next time. The ImGui fallback used to
    // persist whatever was typed, which is the looser meaning this deliberately does not
    // share (SERVER_BROWSER_ARC section 7.8).
    coop::config::WriteIniValue(::coop::config_registry::rows::browser_lastdirect,
                                value.c_str());
    UE_LOGI("browser_input_screens: direct connect accepted -- join_progress owns the "
            "player from here");
    Hide("connecting");
}

void Show(Kind kind) {
    Screen& s = g_screen[Idx(kind)];
    if (!g_switcher || !s.root || s.index < 0) return;
    // A SIBLING REPLACES A SIBLING. If the other input screen (or the browser) is up, the
    // index to restore must be the one BEFORE all of us, not the one we are replacing --
    // otherwise Back walks back into a screen that is no longer listening. The browser
    // solves this by closing synchronously before it opens the hosting window; here the
    // same rule is stated by only recording `priorIndex` when nothing of ours is showing.
    if (g_open < 0) g_priorIndex = U::SwitcherIndex(g_switcher);
    g_open = Idx(kind);
    U::SwitcherSetIndex(g_switcher, s.index);
    g_escPrimed = false;
    g_lmbPrimed = false;
    s.lastStatus.clear();
    SetStatus(s, "", kText);

    // PREFILLED FROM THE ROW IT WRITES. Both values are things the player already has, and
    // retyping a known address to change one digit is the kind of friction a field exists
    // to remove.
    // PREFILLED FROM THE ROW IT WRITES -- except the password, which opens EMPTY on
    // purpose. Both of the others are values the player already owns and retyping them
    // is friction; a password is somebody else's, is not stored anywhere, and a box
    // pre-filled with the LAST server's secret would be a small privacy leak between
    // two lobbies and a confusing failure when it did not work.
    if (kind == Kind::LobbyPassword)      TF::SetText(s.field, std::string());
    else if (kind == Kind::DirectConnect)
        TF::SetText(s.field,
                    coop::config::ResolveString(::coop::config_registry::rows::browser_lastdirect));
    else                                  TF::SetText(s.field, sm::Nickname());
    // FOCUSED ON OPEN. This window exists for one field; making the player click it first
    // would be a step with no decision in it.
    TF::Focus(s.field);
    UE_LOGI("browser_input_screens: shown '%ls' (index %d -> %d)",
            kSpec[Idx(kind)].title, g_priorIndex, s.index);
}

void Hide(const char* why) {
    if (g_open < 0) return;
    Screen& s = g_screen[g_open];
    TF::Blur(s.field);
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == s.index && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("browser_input_screens: hidden (%s; index was %d, ours %d)", why, now, s.index);
    g_open = -1;
}

// BACK RETURNS TO THE BROWSER, not to the main menu. These two windows are only ever
// reached FROM the browser, so the switcher index they replaced is the browser's -- and
// restoring it is not enough on its own, because the browser tracks its own `g_shown` and
// would consider itself closed. Asking it to Open() is the one call that puts both the
// index and that flag back in agreement.
void BackToBrowser() {
    Hide("BACK");
    SB::Open();
}

void PollChrome() {
    if (g_open < 0) return;
    Screen& s = g_screen[g_open];

    // ESC. A FOCUSED FIELD OWNS IT FIRST -- the field turns Escape into "leave the field",
    // and this poll reads the PHYSICAL key (GetAsyncKeyState), so swallowing the message in
    // the detour would not stop this edge. One press must not both blur and close.
    // ASK THE FIELD, not `AnyFocused()`: it blurs on WM_KEYDOWN, so by any edge this poll
    // can take, focus is already gone and the guard could never fire -- one Escape both
    // left the field and closed the window, discarding what was typed. Drained
    // unconditionally so the latch cannot survive into the next press.
    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
    // THE RELEASE EDGE, matching the hosting screens -- it used to be the PRESS edge, and
    // that lost a race it could not win. `GetAsyncKeyState` reflects the physical key
    // immediately, while the latch is set only when `WM_KEYDOWN` is dispatched; this poll
    // runs inside Slate's tick, i.e. AFTER the frame's message pump. A key pressed in the
    // pump->tick gap therefore raised the press edge with the latch still clear, and the
    // window closed while the WM_KEYDOWN arrived a frame later into a blurred field --
    // discarding a typed password roughly half the time, on the password prompt itself.
    // Taking the RELEASE edge puts a whole key-press between the two, so the latch is
    // always set by the time it is read (audit of the fix commit, 2026-08-31).
    const bool escEdge = g_prevEsc && !esc;
    g_prevEsc = esc;
    if (escEdge) {
        if (TF::ConsumeEscape(s.field)) return;   // consumed AT the edge -- see the sibling
        BackToBrowser();
        return;
    }

    // ENTER confirms. The field raises the edge and this consumes it, so the same key that
    // ends typing is the one that acts -- which is what every text field a player has ever
    // used does, and what makes the confirm button optional rather than required.
    if (TF::ConsumeSubmit(s.field)) {
        Confirm(KindOf(g_open));
        return;
    }

    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
    const bool releaseEdge = !down && g_prevLmb;
    g_prevLmb = down;
    if (releaseEdge && ui::input_focus::IsOurWindowForeground()) {
        // IsHovered, and it is right here: these are real UButtons and they answer. The
        // FIELD is not -- it is a hand-built UImage frame and hit-tests by geometry, which
        // is its own Tick's job (native_screen.h states why the mechanism is split by
        // widget KIND).
        if (s.backBtn && E::WidgetIsHovered(s.backBtn)) { BackToBrowser(); return; }
        if (s.okBtn && E::WidgetIsHovered(s.okBtn)) {
            Confirm(KindOf(g_open));
            return;
        }
    }
}

}  // namespace

void Open(Kind kind) {
    g_wantOpen.store(Idx(kind), std::memory_order_relaxed);
    g_wantAtMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

void OpenPasswordPrompt(const std::string& lobbyId, const std::string& displayName,
                        int hostProto, const std::string& hostGame) {
    // The row travels WITH the request; see PendingJoin for why re-reading the
    // selection when OK is pressed would be a different server by then.
    //
    // Written before the intent is published, and read only on the game thread after
    // the intent is consumed -- the same ordering the other two screens rely on, with
    // one writer and one reader and a full tick between them.
    g_pendingJoin.lobbyId     = lobbyId;
    g_pendingJoin.displayName = displayName;
    g_pendingJoin.hostProto   = hostProto;
    g_pendingJoin.hostGame    = hostGame;
    Open(Kind::LobbyPassword);
}

void Close() {
    g_wantOpen.store(-1, std::memory_order_relaxed);
    g_wantClose.store(true, std::memory_order_relaxed);
}

bool IsOpen() { return g_open >= 0; }

// Both windows exist only as doors OFF the native browser, so they follow its flag.
// Without this they were built into every menu instance even with `browser_native=0` --
// two full UUserWidget trees that no code path can reach, on the deliberate ImGui-fallback
// lane (both post-ship audits, 2026-08-31).
bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::browser_native);
    return s;
}

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        for (Screen& s : g_screen) {
            // RELEASE, NOT DESTROY. The widgets died with the menu instance, so the field
            // must unhook its focus and free its handle WITHOUT dispatching RemoveChild
            // into a tree that no longer exists -- which is what `Destroy` does, and what
            // this line did until the post-ship audit read it (2026-08-31). Both sibling
            // screens drop their pointers and touch nothing on this edge; now so does this.
            TF::Release(s.field);
            s = Screen{};
        }
        g_open = -1;
        g_buildAttempts = 0;
    }

    if (!g_screen[0].root || !g_screen[1].root || !g_screen[2].root) {
        // BACKED OFF, because the retry is not free. Each attempt costs a `SwitcherChild`
        // walk (a ChildCount plus a ClassNameOf per child -- an engine call and a wstring
        // EACH, ~13 children here) plus a `DonorField` lookup, and at ~117 menu ticks a
        // second that is well over a thousand dispatches and allocations per second,
        // forever, on exactly the path a version migration lands on. The browser's own
        // builder was given this backoff by the 2026-08-30 perf audit and this one was
        // written without it (2026-08-31 audit, finding F4 reintroduced).
        if (g_toldTheUser) {
            static uint64_t sNextTryMs = 0;
            const uint64_t now = ::GetTickCount64();
            if (now < sNextTryMs) return;
            sNextTryMs = now + 1000;
        }
        void* saveSlots = NS::SwitcherChild(switcher, L"ui_saveSlots_C");
        void* backDonor = NS::DonorField(saveSlots, L"button_back");
        if (!backDonor) {
            // Fail CLOSED and retry. The browser owns the loud player-facing alarm for a
            // missing donor; these screens are reached THROUGH it, so a second dialog would
            // only stack on the first.
            if (++g_buildAttempts == 15) {
                g_toldTheUser = true;
                UE_LOGE("browser_input_screens: ui_saveSlots_C.button_back absent after %d "
                        "attempts -- NOT building the input windows", g_buildAttempts);
            }
            return;
        }
        // A FAILED BUILD COUNTS, so a build that fails for a reason other than a missing
        // donor also backs off instead of re-spawning ~14 UObjects every tick forever.
        if (!g_screen[0].root && !BuildOne(switcher, Kind::DirectConnect, backDonor)) {
            if (++g_buildAttempts >= 15) g_toldTheUser = true;
            return;
        }
        if (!g_screen[1].root && !BuildOne(switcher, Kind::ChangeName, backDonor)) {
            if (++g_buildAttempts >= 15) g_toldTheUser = true;
            return;
        }
        if (!g_screen[2].root && !BuildOne(switcher, Kind::LobbyPassword, backDonor)) {
            if (++g_buildAttempts >= 15) g_toldTheUser = true;
            return;
        }
        UE_LOGI("browser_input_screens: input windows built (direct=%d name=%d password=%d)",
                g_screen[0].index, g_screen[1].index, g_screen[2].index);
    }

    if (g_wantClose.exchange(false, std::memory_order_relaxed)) Hide("requested");
    const int want = g_wantOpen.exchange(-1, std::memory_order_relaxed);
    if (want >= 0) {
        const uint64_t age = ::GetTickCount64() - g_wantAtMs.load(std::memory_order_relaxed);
        if (age <= kIntentTtlMs) {
            // The BROWSER closes first and synchronously, for the reason its own Host
            // handler documents: both screens are children of one switcher, and a sibling
            // that opens on top of a live browser makes the browser's index the one this
            // window will restore.
            SB::CloseNow();
            Show(KindOf(want));
        } else {
            UE_LOGW("browser_input_screens: an open request expired unconsumed after %llu ms",
                    static_cast<unsigned long long>(age));
        }
    }

    if (g_open < 0) return;

    // Reconcile against the LIVE index rather than asserting ours: a sibling screen (or the
    // game's own ESC path) can navigate away, and if it did we were closed, whoever did it.
    if (U::SwitcherIndex(g_switcher) != g_screen[g_open].index) {
        TF::Blur(g_screen[g_open].field);
        g_open = -1;
        return;
    }

    TF::Tick(g_screen[g_open].field);
    PollChrome();
}

}  // namespace ui::browser_input_screens
