// ui/browser_input_screens.cpp -- the three small native windows reached from the browser:
// direct connect (address and password), change name, and the lobby password prompt. See
// ui/browser_input_screens.h.

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

// Small: one label, one field and two buttons, about the shape of the Language window it
// copies.
constexpr float kWindowW = 620.f;
constexpr float kWindowH = 210.f;
// What a second label and field cost, sized generously: the column ends in a spacer that
// absorbs slack, so a window too tall is cosmetic while one too short pushes its footer past
// the bottom edge. There is no fit probe on this window; the margin is the guard.
constexpr float kSecondFieldH = 96.f;
constexpr float kFieldW  = 560.f;

const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kBad    = NS::Bad();

// One screen's widgets.
struct Screen {
    void*      root    = nullptr;
    void*      scrim   = nullptr;
    void*      backBtn = nullptr;
    void*      okBtn   = nullptr;
    void*      status  = nullptr;
    TF::Field* field   = nullptr;
    TF::Field* field2  = nullptr;   // nullptr unless the Spec asks for one
    int32_t    index   = -1;
    std::string lastStatus;
};

Screen  g_screen[3];                 // indexed by Kind

// Which lobby the password prompt is for, captured when Connect was pressed rather than read
// back from the selection at OK: the list re-fetches every 5 s and a refresh re-sorts it, so
// the highlighted row may be a different server by then.
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
bool g_prevTab = false, g_tabPrimed = false;

std::atomic<int>      g_wantOpen{-1};
std::atomic<bool>     g_wantClose{false};
std::atomic<uint64_t> g_wantAtMs{0};
constexpr uint64_t kIntentTtlMs = 20000;

// The inverse of Idx. Two call sites open-coded a two-way ternary, and a third screen would
// have made both confirm the wrong one.
Kind KindOf(int idx) {
    return idx == 0 ? Kind::DirectConnect
                    : (idx == 1 ? Kind::ChangeName : Kind::LobbyPassword);
}

int Idx(Kind k) {
    return k == Kind::DirectConnect ? 0 : (k == Kind::ChangeName ? 1 : 2);
}

// What each screen says, in one table so the screens cannot drift into different idioms.
struct Spec {
    const wchar_t* title;
    const wchar_t* label;
    const wchar_t* hint;
    const wchar_t* confirm;
    int32_t        maxLen;
    // An optional second field; a null label means one box, which is every window but Direct
    // connect. A Spec row rather than a Kind branch in the builder, so the two shapes differ in
    // data, not in construction.
    const wchar_t* label2  = nullptr;
    const wchar_t* hint2   = nullptr;
    int32_t        maxLen2 = 0;
};
const Spec kSpec[3] = {
    // Two boxes: the address, and a password left empty when the server has none. The password cap
    // matches the LobbyPassword window's.
    {L"Multivoid  -  Direct connect", L"Server address", L"host or host:port", L"Connect", 64,
     L"Password  (leave empty if the server has none)", L"password", 64},
    {L"Multivoid  -  Change name",    L"Your name",      L"your name",         L"OK",      24},
    // The cap is 64 codepoints, not the generated length: a host may replace the generated password
    // with anything they can say out loud.
    {L"Multivoid  -  Password",       L"Server password", L"password",         L"Join",    64},
};

// Every field this screen owns, released together. Release is the only thing that clears the
// module's focus pointer, so a leaked focused field leaves the next keystroke dispatching into
// a Field whose UObject died with the menu. One helper, so no teardown path can miss a field.
void ReleaseFields(Screen& s) {
    TF::Release(s.field);
    s.field = nullptr;
    TF::Release(s.field2);
    s.field2 = nullptr;
}

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
    const float windowH = kWindowH + (spec.label2 ? kSecondFieldH : 0.f);
    if (!NS::BuildWindowShell(switcher, kWindowW, windowH, spec.title, shell)) return false;
    void* col = shell.column;

    NS::AddText(col, spec.label, 16, kAccent, NS::kJustLeft, 0.f);
    s.field = TF::Create(col, spec.hint, spec.maxLen, kFieldW);
    if (!s.field) return false;
    if (spec.label2) {
        NS::AddText(col, spec.label2, 16, kAccent, NS::kJustLeft, 0.f);
        s.field2 = TF::Create(col, spec.hint2, spec.maxLen2, kFieldW);
        // The first field is released on the way out: this builder is retried from the menu tick,
        // and a bare return past a successful Create would leak a Field per retry.
        if (!s.field2) { ReleaseFields(s); return false; }
    }

    // The status line under the field starts empty: it says why a confirm did not take.
    s.status = NS::AddText(col, L"", 16, kText, NS::kJustLeft, 0.f);

    // A spacer with all the slack, so the footer sits at the bottom of the window. One weighted
    // empty widget; a Fill slot on the footer would make the footer's own height the window's
    // leftover.
    if (void* spacer = NS::Spawn(L"Spacer", col)) NS::AddVFill(col, spacer, 1.f, NS::kFill, NS::kFill);

    // Footer: Back at the left, the confirm at the right, where every native VOTV window puts its
    // confirm.
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        s.backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        void* gap  = NS::Spawn(L"Spacer", footRow);
        if (gap) NS::AddHFill(footRow, gap, 1.f, NS::kFill, NS::kFill);
        s.okBtn = NS::BuildButton(footRow, backDonor, spec.confirm, NS::kBtnFontPx);
        // Released here too: the next tick rebuilds and mints a second Field, stranding this one in
        // the module's live list.
        if (!s.backBtn || !s.okBtn) { ReleaseFields(s); return false; }
        NS::SetHSlot(NS::SlotOf(s.backBtn), 0.f, NS::kLeft, NS::kCenter);
        NS::SetHSlot(NS::SlotOf(s.okBtn), 0.f, NS::kRight, NS::kCenter);
        NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom);
    }

    s.root = shell.root;
    s.scrim = shell.scrim;
    // The add's return is checked, so an IndexOfChild of -1 has a reason the log can name.
    void* slot = U::AddChild(switcher, s.root);
    s.index = U::IndexOfChild(switcher, s.root);
    if (s.index < 0) {
        UE_LOGE("browser_input_screens: built '%ls' but could NOT place it in the menu "
                "switcher (AddChild slot=%p, GetChildIndex=-1) -- it cannot be shown this "
                "menu", spec.title, slot);
        // The field goes with it: clearing the root alone leaves the heap Field alive and in the
        // live list, and the next tick rebuilds and leaks another. Release, not Destroy: the tree
        // just built is orphaned.
        ReleaseFields(s);
        s.root = nullptr;
        return false;
    }
    return true;
}

void Hide(const char* why);

// The confirm, each screen's one action. The address and the name are written to the ini so
// the answer survives the session.
void Confirm(Kind kind) {
    Screen& s = g_screen[Idx(kind)];
    const std::string value = TF::Text(s.field);
    if (kind == Kind::LobbyPassword) {
        if (value.empty()) {
            SetStatus(s, "Type the password this server was locked with.", kBad);
            return;
        }
        // Handed over, never stored: SetJoinPassword holds it for one join attempt and nothing
        // writes it to the ini, the file people paste into bug reports.
        sm::SetJoinPassword(value);
        if (!sm::JoinLobby(g_pendingJoin.lobbyId, g_pendingJoin.displayName,
                           g_pendingJoin.hostProto, g_pendingJoin.hostGame)) {
            // Dropped on a refusal, so it cannot ride into the next connection the player makes.
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
    // ConnectDirect owns the refusal: it parses the address and answers false for a bad one, so
    // there is no second parser here. The password is this window's second box and is set
    // unconditionally: an empty box writes an empty string, which is both "this server has none"
    // and the clear that stops a value typed for a previous server riding along. An open server
    // ignores whatever is typed: the host sets the password-required flag only when its own lobby
    // password is non-empty, and its check sits inside that branch.
    sm::SetJoinPassword(s.field2 ? TF::Text(s.field2) : std::string());
    if (!sm::ConnectDirect(value)) {
        SetStatus(s, "Could not connect to that address -- check it, or another action is "
                     "already in flight.", kBad);
        return;
    }
    // Written only after the accept gate passed: the row means the last address actually tried and
    // accepted, so a typo cannot overwrite a known-good address.
    coop::config::WriteIniValue(::coop::config_registry::rows::browser_lastdirect,
                                value.c_str());
    UE_LOGI("browser_input_screens: direct connect accepted -- join_progress owns the "
            "player from here");
    Hide("connecting");
}

void Show(Kind kind) {
    Screen& s = g_screen[Idx(kind)];
    if (!g_switcher || !s.root || s.index < 0) return;
    // A sibling replaces a sibling: if the other input screen or the browser is up, the index to
    // restore is the one before all of us, not the one being replaced, or Back walks into a screen
    // no longer listening. priorIndex is recorded only when nothing of ours is showing.
    if (g_open < 0)
        g_priorIndex = NS::SafePriorIndex(U::SwitcherIndex(g_switcher), s.index, g_priorIndex);
    g_open = Idx(kind);
    U::SwitcherSetIndex(g_switcher, s.index);
    g_escPrimed = false;
    g_lmbPrimed = false;
    // The Tab latch too: g_prevTab survives a close, and a Tab held across close-then-reopen would
    // raise a spurious release edge on the first poll.
    g_tabPrimed = false;
    s.lastStatus.clear();
    SetStatus(s, "", kText);

    // Prefilled from the row it writes, except the password, which opens empty: the address and the
    // name are the player's own, while a box carrying the last server's secret is a leak between
    // two lobbies.
    if (kind == Kind::LobbyPassword)      TF::SetText(s.field, std::string());
    else if (kind == Kind::DirectConnect)
        TF::SetText(s.field,
                    coop::config::ResolveString(::coop::config_registry::rows::browser_lastdirect));
    else                                  TF::SetText(s.field, sm::Nickname());
    // The second box opens empty for the same reason.
    if (s.field2) TF::SetText(s.field2, std::string());
    // Focused on open, on the box the player always fills in. Tab moves to the other; so does a
    // click, since the field hit-tests its own box by geometry.
    TF::Focus(s.field);
    UE_LOGI("browser_input_screens: shown '%ls' (index %d -> %d)",
            kSpec[Idx(kind)].title, g_priorIndex, s.index);
}

void Hide(const char* why) {
    if (g_open < 0) return;
    Screen& s = g_screen[g_open];
    TF::Blur(s.field);
    if (s.field2) TF::Blur(s.field2);
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == s.index && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("browser_input_screens: hidden (%s; index was %d, ours %d)", why, now, s.index);
    g_open = -1;
}

// Back returns to the browser, not the main menu: these windows are only reached from it.
// Restoring the index alone is not enough, since the browser tracks its own shown flag; Open
// puts both back in agreement.
void BackToBrowser() {
    Hide("BACK");
    SB::Open();
}

void PollChrome() {
    if (g_open < 0) return;
    Screen& s = g_screen[g_open];

    // Escape: a focused field owns it first, turning it into "leave the field". This poll reads the
    // physical key, so swallowing the message in the detour would not stop the edge. The field is
    // asked, not AnyFocused: the field blurs on key-down, so by any edge this poll sees, focus is
    // already gone.
    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
    // The release edge, as the hosting screens use. The physical key reads immediately, the field's
    // latch is set only when the key-down message is dispatched, and this poll runs after the
    // frame's message pump; a press edge could fire with the latch still clear and close the window
    // while the key-down landed a frame later in a blurred field, discarding the typed password. A
    // release edge puts a whole key-press between the two.
    const bool escEdge = g_prevEsc && !esc;
    g_prevEsc = esc;
    if (escEdge) {
        // Either field may own the Escape; asking only the first would let a press inside the
        // password box both leave it and close the window.
        if (TF::ConsumeEscape(s.field)) return;   // consumed at the edge
        if (s.field2 && TF::ConsumeEscape(s.field2)) return;
        BackToBrowser();
        return;
    }

    // Tab moves between the two boxes, the keyboard route beside the field's own click hit-test.
    // Release edge and a priming pass, for the same pump-to-tick race as Escape.
    if (s.field2) {
        const bool tab = (::GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
        if (!g_tabPrimed) { g_tabPrimed = true; g_prevTab = tab; }
        const bool tabEdge = g_prevTab && !tab;
        g_prevTab = tab;
        if (tabEdge) {
            const bool onFirst = TF::Focused(s.field);
            TF::Blur(s.field);
            TF::Blur(s.field2);
            TF::Focus(onFirst ? s.field2 : s.field);
            return;
        }
    }

    // Enter confirms: the field raises the edge and this consumes it, so the confirm button is
    // optional.
    if (s.field2 && TF::ConsumeSubmit(s.field2)) { Confirm(KindOf(g_open)); return; }
    if (TF::ConsumeSubmit(s.field)) {
        Confirm(KindOf(g_open));
        return;
    }

    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
    const bool releaseEdge = !down && g_prevLmb;
    g_prevLmb = down;
    if (releaseEdge && ui::input_focus::IsOurWindowForeground()) {
        // IsHovered: these are real UButtons and they answer. The field is a hand-built image frame
        // that hit-tests by geometry in its own Tick.
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
    // The row travels with the request (see PendingJoin). Written before the intent is published
    // and read on the game thread after the intent is consumed: one writer, one reader, a tick
    // between them.
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

// These windows exist only as doors off the native browser, so they follow its flag; otherwise
// they would be built into every menu instance with the browser disabled, unreachable.
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
            // Release, not Destroy: the widgets died with the menu instance, so the field must
            // unhook its focus and free its handle without dispatching RemoveChild into a tree that
            // no longer exists.
            ReleaseFields(s);
            s = Screen{};
        }
        g_open = -1;
        g_buildAttempts = 0;
    }

    if (!g_screen[0].root || !g_screen[1].root || !g_screen[2].root) {
        // Backed off, since the retry is not free: each attempt walks the switcher's children with
        // an engine call and a string per child, plus a donor lookup, every menu tick, on exactly
        // the path a version migration lands on.
        if (g_toldTheUser) {
            static uint64_t sNextTryMs = 0;
            const uint64_t now = ::GetTickCount64();
            if (now < sNextTryMs) return;
            sNextTryMs = now + 1000;
        }
        void* saveSlots = NS::SwitcherChild(switcher, L"ui_saveSlots_C");
        void* backDonor = NS::DonorField(saveSlots, L"button_back");
        if (!backDonor) {
            // Fail closed and retry. The browser owns the loud alarm for a missing donor; these
            // screens are reached through it, so a second dialog would only stack on the first.
            if (++g_buildAttempts == 15) {
                g_toldTheUser = true;
                UE_LOGE("browser_input_screens: ui_saveSlots_C.button_back absent after %d "
                        "attempts -- NOT building the input windows", g_buildAttempts);
            }
            return;
        }
        // A failed build counts, so a build that fails for another reason also backs off instead of
        // re-spawning widgets every tick.
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
            // The browser closes first, synchronously: both are children of one switcher, and a
            // sibling opening on top of a live browser would restore the browser's index.
            SB::CloseNow();
            Show(KindOf(want));
        } else {
            UE_LOGW("browser_input_screens: an open request expired unconsumed after %llu ms",
                    static_cast<unsigned long long>(age));
        }
    }

    // Reconciled against the live index in both directions. Closed is observed below; open is
    // observed here, so a caller that restores one of these screens by index gets a live screen.
    if (g_open < 0) {
        const int32_t live = NS::ActiveIndex();
        if (live < 0) return;
        for (int i = 0; i < 3; ++i) {
            if (!g_screen[i].root || g_screen[i].index != live) continue;
            g_open = i;
            g_escPrimed = false;
            g_lmbPrimed = false;
            g_prevTab   = false;
            UE_LOGI("browser_input_screens: live again (index %d returned to screen %d)",
                    live, i);
            break;
        }
        if (g_open < 0) return;
    }

    if (NS::ActiveIndex() != g_screen[g_open].index) {
        // Both, as Hide does: blurring only the first left the focus on the password box with
        // nothing ticking it, so every keystroke landed in an invisible box and Escape could not
        // close the browser.
        TF::Blur(g_screen[g_open].field);
        TF::Blur(g_screen[g_open].field2);
        g_open = -1;
        return;
    }

    // Both fields tick: only the focused one animates a caret, but an unticked field never sees its
    // own state advance.
    TF::Tick(g_screen[g_open].field);
    if (g_screen[g_open].field2) TF::Tick(g_screen[g_open].field2);
    PollChrome();
}

}  // namespace ui::browser_input_screens
