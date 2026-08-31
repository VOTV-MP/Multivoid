// ui/server_browser_actions.cpp -- see ui/server_browser_actions.h.

#include "ui/server_browser_actions.h"

#include "ui/server_browser_native.h"   // the selection these act on, and the notice line
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

}  // namespace

bool Build(void* parent, void* donorBtn) {
    if (!parent) return false;
    // THE CELL TABLE. Left to right, top to bottom, in the order a player moves through
    // them: join the one you picked, open your own, refresh what you are looking at.
    //
    // SENTENCE CASE, NEVER CAPS. Measured across the whole style corpus
    // (ignore_folder/votv_widgets_style/): VOTV uppercases NO button label,
    // anywhere -- "Play game", "Delete save slot", "Open save data reset menu",
    // "Duplicate save slot", "Back", "Save", "Reset". Ours shouted, which is the
    // single loudest way our chrome read as foreign. User report 2026-08-30:
    // "No caps at buttons ever."
    //
    // TWO CELLS ARE MISSING ON PURPOSE, and they are the ones the input-variant fork owns:
    // "Direct connect" and "Change name". The user's answer to fork P1 was to BUILD BOTH
    // input designs and choose by eye ("попробуем разные дизайны и что лучше будет то и
    // оставим"), so where those two live is exactly what is being compared -- variant A
    // gives them cells here that open sibling screens, variant B puts the input in the
    // browser itself. A dead button that says "not built yet" would be neither, and this
    // project does not ship those.
    struct Cell { const wchar_t* label; void** out; };
    const Cell cells[] = {
        {L"Connect",     &g_connect},
        {L"Host game",   &g_host},
        {L"Update list", &g_refresh},
    };
    constexpr int   kPerRow = 3;
    constexpr float kRowH   = 46.f;   // large, the way the save browser's action block is
    void* row = nullptr;
    const int n = static_cast<int>(sizeof(cells) / sizeof(cells[0]));
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
    return false;
}

void Forget() {
    g_connect = g_host = g_refresh = nullptr;
}

void* HostButton()    { return g_host; }
void* ConnectButton() { return g_connect; }
void* RefreshButton() { return g_refresh; }

const char* LastOutcome() { return g_lastOutcome; }

}  // namespace ui::server_browser_actions
