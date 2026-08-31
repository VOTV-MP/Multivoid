// ui/server_browser_actions.cpp -- see ui/server_browser_actions.h.

#include "ui/server_browser_actions.h"

#include "ui/server_browser_native.h"   // the selection these act on, and the notice line
#include "ui/browser_input_screens.h"   // where the two input doors lead
#include "ui/host_window_native.h"      // what HOST opens
#include "ui/native_screen.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"           // BuildButton + the palette

#include "coop/net/lobby_client.h"
#include "coop/session/session_manager.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <string>

namespace ui::server_browser_actions {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace sm = coop::session_manager;
namespace SB = ui::server_browser_native;

void* g_connect = nullptr;
void* g_host    = nullptr;
void* g_refresh = nullptr;
void* g_direct  = nullptr;
void* g_rename  = nullptr;

// The last decision a handled click reached -- see the header. A pointer to a string
// LITERAL, so it needs no storage and cannot dangle.
const char* g_lastOutcome = "";

// CONNECT, and every way it can decline.
//
// The decline paths are sentences, not disabled buttons. A greyed-out control tells a
// player that something is wrong and not what -- and our buttons are style CLONES of the
// game's own, so their disabled appearance is whatever the donor happens to carry rather
// than something we chose. Saying it in the footer costs one line and answers the question.
void DoConnect() {
    coop::net::lobby::LobbyRow row;
    if (!SB::SelectedRow(row)) {
        g_lastOutcome = "connect:none";
        SB::SetNotice("Pick a server from the list first.");
        return;
    }
    // A host cannot join itself. `JoinLobby` refuses this too, so this is the message
    // rather than the guard -- the guard lives at the one place that can enforce it.
    if (!row.lobbyId.empty() && row.lobbyId == sm::OwnLobbyId()) {
        g_lastOutcome = "connect:self";
        SB::SetNotice("That's your own server -- you're already hosting it.");
        return;
    }
    // A LOCKED ROW GOES TO THE PASSWORD PROMPT INSTEAD OF STRAIGHT AT THE HOST, which
    // is what the user asked for: "если кто хочет к серверу из списка с замком
    // подключиться и версия правильная то когда он нажмет connect в правой PANE,
    // вылезет отдельное окно ввода пароля" (2026-08-31).
    //
    // "И ВЕРСИЯ ПРАВИЛЬНАЯ" IS ALREADY HANDLED AND IS NOT RE-CHECKED HERE. `JoinLobby`
    // owns the version-equality gate and refuses with the connect-failed popup, per the
    // "show normally, reject on Join" policy -- and the prompt hands the row straight
    // back to that same call, so a mismatched locked server still refuses for the
    // version, one window later. A second copy of that gate here would be two opinions
    // about what a joinable build is.
    //
    // The row is captured BY VALUE into the prompt: five seconds of typing is a
    // re-fetch and a re-sort, and the selection can be a different server by the time
    // OK is pressed.
    if (row.locked) {
        g_lastOutcome = "connect:password";
        SB::CloseNow();   // sibling hand-over, exactly as HOST does
        ui::browser_input_screens::OpenPasswordPrompt(row.lobbyId, row.name, row.proto,
                                                      row.game);
        return;
    }
    // NOT A LOCKED SERVER, so anything left over from a previous prompt must not ride
    // along -- a stale password would be sent to a host that never asked for one.
    sm::SetJoinPassword("");
    // The version pair rides along so the EQUALITY gate can refuse HERE, with the
    // connect-failed popup, rather than letting the wire gate drop the player later
    // (the "show normally, reject on Join" policy, session_manager.h:118).
    if (sm::JoinLobby(row.lobbyId, row.name, row.proto, row.game)) {
        g_lastOutcome = "connect:started";
        SB::Close();   // accepted: the loading screen owns the player from here
    } else {
        g_lastOutcome = "connect:busy";
        SB::SetNotice("Could not start that connection -- another action is already "
                      "in flight.");
    }
}

// HOST opens the native host window, which is where the world and the connection type are
// chosen. It does NOT host anything itself: there is exactly one host action in the tree
// (`session_manager::HostWithSave`) and that window is what calls it.
//
// The browser closes FIRST and SYNCHRONOUSLY, and the order is the whole correctness of
// this function. Both screens are children of one switcher, and the window records the
// index it replaces so its Back can restore it -- so the browser must already be gone when
// the window opens, or the window's Back returns the player to a browser that is no longer
// listening and cannot be closed. `CloseNow` states that ordering instead of leaving it to
// which observer happens to tick first; see its declaration for the measurement.
//
// KNOWN AND DELIBERATE: this is a one-way door. Back from the hosting window returns to the
// MAIN MENU, not to the browser -- the same behaviour every native VOTV sub-screen has.
void DoHost() {
    g_lastOutcome = "host";
    SB::CloseNow();
    ui::host_window_native::Open();
}

void DoRefresh() {
    g_lastOutcome = "refresh";
    sm::Refresh();
    SB::SetNotice("Refreshing the server list...");
}

// THE TWO INPUT DOORS. They author nothing themselves -- the sibling window owns the value,
// the validation and the ini write -- so a click here is only navigation.
//
// THE BROWSER ITSELF HAS NO TEXT ENTRY, and that is the user's call, stated three times
// and finally without conditions: "не нужен прям в нем ввод", then "your name and connect
// by address don't belong on the right panel with the server info - that takes too much
// space", then "не нужны эти панели ввода внизу сервер браузера". An inline strip was
// built as the competing candidate and DELETED under RULE 2 the same session
// (`ui/server_browser_inline_input`, recoverable from history). The browser lists servers
// and acts on one; typing happens in its own window.
void DoDirect() {
    g_lastOutcome = "direct";
    ui::browser_input_screens::Open(ui::browser_input_screens::Kind::DirectConnect);
}
void DoRename() {
    g_lastOutcome = "rename";
    ui::browser_input_screens::Open(ui::browser_input_screens::Kind::ChangeName);
}

}  // namespace

bool BuildConnect(void* parent, void* donorBtn) {
    if (!parent) return false;
    // ONE BUTTON, UNDER THE PANEL THAT DESCRIBES WHAT IT WILL DO (USER 2026-08-31: "it
    // makes more sense for the Connect button to appear somewhere in the right panel under
    // server info"). It is the CONFIRM of this screen, so it is the widest and it sits
    // alone -- the details panel names a server and this is the sentence's verb.
    void* rowBox = NS::Spawn(L"SizeBox", parent);
    void* row    = rowBox ? NS::Spawn(L"HorizontalBox", rowBox) : nullptr;
    if (!rowBox || !row) return false;
    U::SetSizeBoxHeight(rowBox, 46.f);
    U::SetContent(rowBox, row);
    if (void* s = NS::AddVFill(parent, rowBox, 0.f, NS::kFill, NS::kTop))
        NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, 6.f);
    g_connect = NS::BuildButton(row, donorBtn, L"Connect", NS::kBtnFontPx);
    if (!g_connect) {
        UE_LOGE("server_browser_actions: could not build CONNECT -- the screen would have "
                "no way to join the server it is describing");
        return false;
    }
    NS::SetHSlot(NS::SlotOf(g_connect), 1.f, NS::kFill, NS::kFill);
    return true;
}

bool Build(void* parent, void* donorBtn) {
    if (!parent) return false;
    // THE CELL TABLE -- the actions that are NOT about the selected row. CONNECT is not
    // here: it acts on whatever the player picked, and what they picked is described in the
    // details panel, so it lives directly under that panel instead (USER 2026-08-31: "it
    // makes more sense for the Connect button to appear somewhere in the right panel under
    // server info"). What is left in the grid is everything that is true regardless of the
    // selection -- connect by address, host your own, rename yourself, refetch the list --
    // which is a cleaner split than "all the buttons in one block" ever was.
    //
    // SENTENCE CASE, NEVER CAPS. Measured across the whole style corpus
    // (ignore_folder/votv_widgets_style/): VOTV uppercases NO button label,
    // anywhere -- "Play game", "Delete save slot", "Open save data reset menu",
    // "Duplicate save slot", "Back", "Save", "Reset". Ours shouted, which is the
    // single loudest way our chrome read as foreign. User report 2026-08-30:
    // "No caps at buttons ever."
    //
    // CONNECT is not here -- it acts on the selected row and lives under the panel that
    // describes it (BuildConnect above, USER 2026-08-31). What is left is everything that
    // is true regardless of which server is picked.
    struct Cell { const wchar_t* label; void** out; };
    const Cell cells[] = {
        {L"Direct connect", &g_direct},
        {L"Host game",      &g_host},
        {L"Change name",    &g_rename},
        {L"Update list",    &g_refresh},
    };
    const int cellCount = static_cast<int>(sizeof(cells) / sizeof(cells[0]));

    // THE ROW WIDTH DIVIDES THE CELLS EVENLY, it is not a constant. At a fixed 3 per row,
    // four cells render as a row of three and then ONE button stretched across the whole
    // width -- which reads as a mistake rather than as a grid (measured by eye,
    // browser_row_skin_a.png 2026-08-31). Four go 2x2; anything else keeps three.
    const int   kPerRow = (cellCount == 4) ? 2 : (cellCount < 3 ? cellCount : 3);
    constexpr float kRowH   = 46.f;   // large, the way the save browser's action block is
    void* row = nullptr;
    const int n = cellCount;
    for (int i = 0; i < n; ++i) {
        if (i % kPerRow == 0) {
            // THE HEIGHT IS THE ROW'S, NOT EACH CELL'S. A SizeBox per button would mean
            // handing `BuildButton` a SizeBox as its parent -- and BuildButton attaches its
            // own child and then writes UHorizontalBoxSlot offsets into whatever slot it
            // got, which on a USizeBoxSlot is a wrong-offset write into a neighbouring
            // field (docs/LESSONS.md: it never faults where you wrote it). One SizeBox
            // around the row gives every button in it the same height for free.
            void* rowBox = NS::Spawn(L"SizeBox", parent);
            row = rowBox ? NS::Spawn(L"HorizontalBox", rowBox) : nullptr;
            if (!rowBox || !row) return false;
            U::SetSizeBoxHeight(rowBox, kRowH);
            U::SetContent(rowBox, row);
            if (void* s = NS::AddVFill(parent, rowBox, 0.f, NS::kFill, NS::kTop))
                NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, 4.f);
        }
        void* btn = NS::BuildButton(row, donorBtn, cells[i].label, NS::kBtnFontPx);
        if (!btn) {
            UE_LOGE("server_browser_actions: could not build the '%ls' action -- the grid "
                    "would ship with a hole", cells[i].label);
            return false;
        }
        // BuildButton centres its button in an auto-sized slot, which is right for a footer
        // bar and wrong for a grid: the cells must be equal and must fill the row. Its slot
        // is reconfigured rather than re-created -- the widget is already attached.
        void* s = NS::SlotOf(btn);
        NS::SetHSlot(s, 1.f, NS::kFill, NS::kFill);
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding,
                           0.f, 0.f, (i % kPerRow == kPerRow - 1 || i == n - 1) ? 0.f : 4.f,
                           0.f);
        *cells[i].out = btn;
    }
    return true;
}

bool OnReleaseEdge() {
    // Order matches nothing in particular -- the three rects do not overlap, so at most one
    // can answer. First hit wins and stops.
    // IsHovered on real UButtons -- see the note in server_browser_native's chrome poll.
    // These passed on IsHovered before and after a brief geometry conversion; the X did
    // not survive it, so the mechanism stays split by WIDGET KIND, not by screen.
    if (g_connect && E::WidgetIsHovered(g_connect)) { DoConnect(); return true; }
    if (g_host    && E::WidgetIsHovered(g_host))    { DoHost();    return true; }
    if (g_refresh && E::WidgetIsHovered(g_refresh)) { DoRefresh(); return true; }
    if (g_direct  && E::WidgetIsHovered(g_direct))  { DoDirect();  return true; }
    if (g_rename  && E::WidgetIsHovered(g_rename))  { DoRename();  return true; }
    return false;
}

void Forget() {
    g_connect = g_host = g_refresh = g_direct = g_rename = nullptr;
}

void* HostButton()    { return g_host; }
void* ConnectButton() { return g_connect; }
void* RefreshButton() { return g_refresh; }

const char* LastOutcome() { return g_lastOutcome; }

}  // namespace ui::server_browser_actions
