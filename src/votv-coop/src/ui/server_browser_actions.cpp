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

// The last decision a handled click reached (see the header). A pointer to a string literal,
// so it needs no storage and cannot dangle.
const char* g_lastOutcome = "";

// Connect, and every way it can decline. The decline paths are sentences, not disabled
// buttons: a greyed-out control says something is wrong and not what, and our buttons are
// style clones of the game's own, so their disabled look is whatever the donor carries.
void DoConnect() {
    coop::net::lobby::LobbyRow row;
    if (!SB::SelectedRow(row)) {
        g_lastOutcome = "connect:none";
        SB::SetNotice("Pick a server from the list first.");
        return;
    }
    // A host cannot join itself. The join call refuses this too, so this is the message rather
    // than the guard; the guard lives at the one place that can enforce it.
    if (!row.lobbyId.empty() && row.lobbyId == sm::OwnLobbyId()) {
        g_lastOutcome = "connect:self";
        SB::SetNotice("That's your own server -- you're already hosting it.");
        return;
    }
    // A locked row goes to the password prompt instead of straight at the host. The version is
    // not re-checked here: the join call owns the version-equality gate and refuses with the
    // connect-failed popup, and the prompt hands the row straight back to that same call, so a
    // mismatched locked server still refuses for the version, one window later. The row is
    // captured by value into the prompt: five seconds of typing is a re-fetch and a re-sort,
    // and the selection can be a different server by the time OK is pressed.
    if (row.locked) {
        g_lastOutcome = "connect:password";
        SB::CloseNow();   // sibling hand-over, exactly as HOST does
        ui::browser_input_screens::OpenPasswordPrompt(row.lobbyId, row.name, row.proto,
                                                      row.game);
        return;
    }
    // Not a locked server, so anything left over from a previous prompt must not ride along: a
    // stale password would be sent to a host that never asked for one.
    sm::SetJoinPassword("");
    // The version pair rides along so the equality gate can refuse here, with the connect-failed
    // popup, rather than letting the wire gate drop the player later (rows show normally and are
    // rejected on join).
    if (sm::JoinLobby(row.lobbyId, row.name, row.proto, row.game)) {
        g_lastOutcome = "connect:started";
        SB::Close();   // accepted: the loading screen owns the player from here
    } else {
        g_lastOutcome = "connect:busy";
        SB::SetNotice("Could not start that connection -- another action is already "
                      "in flight.");
    }
}

// Host opens the native host window, where the world and the connection type are chosen. It
// hosts nothing itself: the one host action in the tree is the session manager's host call,
// and that window is what makes it. The browser closes first and synchronously, and that
// order is the whole correctness of this function: both screens are children of one
// switcher, and the window records the index it replaces so its Back can restore it, so the
// browser must already be gone when the window opens, or Back returns the player to a
// browser that is no longer listening and cannot be closed (see CloseNow's declaration).
// Deliberately a one-way door: Back from the hosting window returns to the main menu, not
// to the browser, the same behaviour every native sub-screen has.
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

// The two input doors. They author nothing themselves: the sibling window owns the value,
// the validation and the ini write, so a click here is only navigation. The browser itself
// has no text entry; it lists servers and acts on one, and typing happens in its own window.
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
    // One button, under the panel that describes what it will do. It is the confirm of this
    // screen, so it is the widest and it sits alone: the details panel names a server and this
    // is the sentence's verb.
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
    // The cell table: the actions that are not about the selected row. Connect is not here; it
    // acts on whatever the player picked, which the details panel describes, so it lives under
    // that panel. What is left is everything true regardless of the selection: connect by
    // address, host your own, rename yourself, refetch the list. Sentence case, never caps: the
    // game uppercases no button label anywhere, and shouting was the single loudest way our
    // chrome read as foreign.
    struct Cell { const wchar_t* label; void** out; };
    const Cell cells[] = {
        {L"Direct connect", &g_direct},
        {L"Host game",      &g_host},
        {L"Change name",    &g_rename},
        {L"Update list",    &g_refresh},
    };
    const int cellCount = static_cast<int>(sizeof(cells) / sizeof(cells[0]));

    // The row width divides the cells evenly; it is not a constant. At a fixed three per row,
    // four cells render as a row of three and then one button stretched across the whole width,
    // which reads as a mistake rather than a grid. Four go two by two; anything else keeps three.
    const int   kPerRow = (cellCount == 4) ? 2 : (cellCount < 3 ? cellCount : 3);
    constexpr float kRowH   = 46.f;   // large, the way the save browser's action block is
    void* row = nullptr;
    const int n = cellCount;
    for (int i = 0; i < n; ++i) {
        if (i % kPerRow == 0) {
            // The height is the row's, not each cell's. A size box per button would hand
            // BuildButton a size box as its parent, and BuildButton attaches its own child and then
            // writes horizontal box slot offsets into whatever slot it got, which on a size box
            // slot is a wrong-offset write into a neighbouring field. One size box around the row
            // gives every button in it the same height.
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
        // BuildButton centres its button in an auto-sized slot, right for a footer bar and wrong
        // for a grid: the cells must be equal and must fill the row. Its slot is reconfigured
        // rather than re-created, since the widget is already attached.
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
    // Order matches nothing in particular: the rects do not overlap, so at most one can answer.
    // First hit wins and stops. Hover on real buttons (see the note in the browser's chrome
    // poll); the mechanism is split by widget kind, not by screen.
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
