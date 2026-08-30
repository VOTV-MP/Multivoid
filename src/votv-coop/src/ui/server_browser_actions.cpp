// ui/server_browser_actions.cpp -- see ui/server_browser_actions.h.

#include "ui/server_browser_actions.h"

#include "ui/server_browser_native.h"   // the selection these act on, and the notice line
#include "ui/host_window_native.h"      // what HOST opens
#include "ui/native_screen.h"           // BuildButton + the palette

#include "coop/net/lobby_client.h"
#include "coop/session/session_manager.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

namespace ui::server_browser_actions {
namespace {

namespace E  = ue_wrap::engine;
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

bool Build(void* footRow, void* donorBtn) {
    if (!footRow) return false;
    // Order left-to-right, and it is the order a player moves through them: refresh the
    // list, host your own, or join the one you picked. CONNECT is last so it sits at the
    // right edge, which is the CONFIRM position in every native VOTV window.
    g_refresh = NS::BuildButton(footRow, donorBtn, L"REFRESH", 18);
    g_host    = NS::BuildButton(footRow, donorBtn, L"HOST",    18);
    g_connect = NS::BuildButton(footRow, donorBtn, L"CONNECT", 18);
    if (!g_refresh || !g_host || !g_connect) {
        UE_LOGE("server_browser_actions: could not build the action bar "
                "(refresh=%p host=%p connect=%p) -- the footer would ship with a hole",
                g_refresh, g_host, g_connect);
        return false;
    }
    return true;
}

bool OnReleaseEdge() {
    // Order matches nothing in particular -- the three rects do not overlap, so at most one
    // can answer. First hit wins and stops.
    if (g_connect && E::WidgetIsHovered(g_connect)) { DoConnect(); return true; }
    if (g_host    && E::WidgetIsHovered(g_host))    { DoHost();    return true; }
    if (g_refresh && E::WidgetIsHovered(g_refresh)) { DoRefresh(); return true; }
    return false;
}

void Forget() { g_connect = g_host = g_refresh = nullptr; }

void* HostButton()    { return g_host; }
void* ConnectButton() { return g_connect; }
void* RefreshButton() { return g_refresh; }

const char* LastOutcome() { return g_lastOutcome; }

}  // namespace ui::server_browser_actions
