// ui/server_browser_actions.cpp -- see ui/server_browser_actions.h.

#include "ui/server_browser_actions.h"

#include "ui/server_browser_native.h"   // the selection these act on, and the notice line
#include "ui/host_window_native.h"      // what HOST opens
#include "ui/native_screen.h"
#include "ui/native_text_field.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"           // BuildButton + the palette

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
ui::native_text_field::Field* g_addr = nullptr;   // the direct-IP box (MTA's per-tab address edit)
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
    // AN ADDRESS BEATS A SELECTION, and the two share one button on purpose -- MTA wires
    // its address edit and its Connect button to the SAME `OnConnectClick`
    // (`CServerBrowser.cpp:489`). A second button would ask the player which kind of
    // connecting they are doing, which is not a question they have.
    //
    // This is the door the user reported missing: "нету возможности нигде по айпи
    // подключиться - НИГДЕ" (2026-08-30). `ConnectDirect` has always worked; it was
    // reachable only from the ImGui browser, which stopped being the default that day.
    if (g_addr) {
        const std::string addr = ui::native_text_field::Text(g_addr);
        if (!addr.empty()) {
            // Remembered across launches, the same row the ImGui browser has always
            // written -- so a player who used one surface finds their address in the other.
            coop::config::WriteIniValue(::coop::config_registry::rows::browser_lastdirect,
                                        addr.c_str());
            if (sm::ConnectDirect(addr)) {
                g_lastOutcome = "connect:direct";
                SB::Close();
            } else {
                g_lastOutcome = "connect:badaddr";
                SB::SetNotice("That address could not be used -- expected host or host:port.");
            }
            return;
        }
    }
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
    // SENTENCE CASE, NEVER CAPS. Measured across the whole style corpus
    // (ignore_folder/votv_widgets_style/): VOTV uppercases NO button label,
    // anywhere -- "Play game", "Delete save slot", "Open save data reset menu",
    // "Duplicate save slot", "Back", "Save", "Reset". Ours shouted, which is the
    // single loudest way our chrome read as foreign. User report 2026-08-30:
    // "No caps at buttons ever."
    // THE ADDRESS BOX FIRST, at the left of the actions: a player reads it before the
    // buttons that act on it. It carries its own input because Slate will not deliver
    // keystrokes into this tree -- measured, see ui/native_text_field.h.
    g_addr = ui::native_text_field::Create(footRow, L"Enter an address [IP:Port]", 64, 300.f);
    if (g_addr) {
        // Seed from the remembered address so the common case -- reconnecting to the same
        // friend -- is one click rather than one retype.
        const std::string last =
            coop::config::ResolveString(::coop::config_registry::rows::browser_lastdirect);
        if (!last.empty() && last != "127.0.0.1:7777")
            ui::native_text_field::SetText(g_addr, last);
    } else {
        UE_LOGW("server_browser_actions: the address field would not build -- direct-IP "
                "connect is unreachable on this surface again, which is the exact defect "
                "the 2026-08-30 report named. The list still works.");
    }
    g_refresh = NS::BuildButton(footRow, donorBtn, L"Refresh", NS::kBtnFontPx);
    g_host    = NS::BuildButton(footRow, donorBtn, L"Host",    NS::kBtnFontPx);
    g_connect = NS::BuildButton(footRow, donorBtn, L"Connect", NS::kBtnFontPx);
    if (!g_refresh || !g_host || !g_connect) {
        UE_LOGE("server_browser_actions: could not build the action bar "
                "(refresh=%p host=%p connect=%p) -- the footer would ship with a hole",
                g_refresh, g_host, g_connect);
        return false;
    }
    return true;
}

// Per-tick, from the browser's own observer. Drives the address box's caret and its
// click-to-focus, and turns Enter into the same action the Connect button performs --
// MTA's `SetTextAcceptedHandler(OnConnectClick)` shape, where committing the text and
// pressing the button are one path rather than two that can drift.
void Tick() {
    if (!g_addr) return;
    ui::native_text_field::Tick(g_addr);
    if (ui::native_text_field::ConsumeSubmit(g_addr)) DoConnect();
}

bool OnReleaseEdge() {
    // Order matches nothing in particular -- the three rects do not overlap, so at most one
    // can answer. First hit wins and stops.
    if (g_connect && E::WidgetIsHovered(g_connect)) { DoConnect(); return true; }
    if (g_host    && E::WidgetIsHovered(g_host))    { DoHost();    return true; }
    if (g_refresh && E::WidgetIsHovered(g_refresh)) { DoRefresh(); return true; }
    return false;
}

void Forget() {
    // The field owns a heap handle and a slot in the focus registry, so it is DESTROYED
    // rather than forgotten -- dropping the pointer would leak it and leave a dead entry
    // that the WndProc seam still walks.
    ui::native_text_field::Destroy(g_addr);
    g_addr = nullptr;
    g_connect = g_host = g_refresh = nullptr;
}

void* HostButton()    { return g_host; }
void* ConnectButton() { return g_connect; }
void* RefreshButton() { return g_refresh; }

const char* LastOutcome() { return g_lastOutcome; }

}  // namespace ui::server_browser_actions
