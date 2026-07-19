// ui/server_browser.cpp -- see ui/server_browser.h.
//
// MULTIPLAYER server browser, rendered as an ImGui modal over VOTV's main menu.
// Row model ported from MTA's CServerListItem (reference/mtasa-blue/Client/core/
// ServerBrowser/CServerList.h): name, players cur/max, version, world, locked. The
// LIVE feed is coop::session_manager (master GET /v1/lobbies via lobby_client); the
// Connect / Host / Direct-IP controls drive coop::session_manager (which announces /
// joins on a worker thread and queues a coop::net::Config the harness boots). No
// ping column: per the connectivity-ladder design, ping is measured post-connect via
// GNS, not pre-listed (MTA's ASE-UDP per-server query is intentionally dropped) -- we
// show the lobby's heartbeat age instead.

#include "ui/server_browser.h"

#include "ui/host_save_picker.h"
#include "ui/menu_sfx.h"        // native VOTV button click + rollover sounds
#include "coop/net/protocol.h"  // kProtocolVersion (the v59 "Ver" mismatch tint)
#include "coop/session/session_manager.h"
#include "coop/config/config.h"   // local-only ini persistence for the name + last direct address
#include "ui/scale.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ui::server_browser {
namespace {

namespace sm = coop::session_manager;
using Row = coop::net::lobby::LobbyRow;
using ui::scale::S;

std::atomic<bool> g_open{false};
std::atomic<bool> g_justOpened{false};   // Open() -> Render triggers one auto-refresh

// Render-thread-only UI state.
std::vector<Row> g_rows;
int  g_selected = -1;
char g_directIp[64] = "127.0.0.1:7777";
char g_hostName[64] = "My VOTV Server";
char g_nick[64] = "Player";   // local display name; loaded from session_manager on open
bool g_hostLocked = false;

}  // namespace

void Open() {
    g_open.store(true, std::memory_order_relaxed);
    g_justOpened.store(true, std::memory_order_relaxed);
}
void Close() { g_open.store(false, std::memory_order_relaxed); }
void Toggle(){ g_open.store(!g_open.load(std::memory_order_relaxed), std::memory_order_relaxed); }
bool IsOpen(){ return g_open.load(std::memory_order_relaxed); }

void Render() {
    if (!IsOpen()) return;
    ui::menu_sfx::FrameBegin();  // arm per-frame hover-enter detection for the sfx buttons

    // Auto-refresh once when the browser is opened (so the list isn't stale/empty) + load
    // the current nickname (config default, or whatever the user last set) into the field.
    if (g_justOpened.exchange(false, std::memory_order_relaxed)) {
        sm::Refresh();
        std::snprintf(g_nick, sizeof(g_nick), "%s", sm::Nickname().c_str());
        // Restore the last direct-connect address the user typed (persisted locally
        // in multivoid.ini). Falls back to the loopback default on a fresh install.
        std::snprintf(g_directIp, sizeof(g_directIp), "%s",
                      coop::config::ReadIniValue("browser.lastdirect", "127.0.0.1:7777").c_str());
    }
    // Pull the latest fetched rows (cheap copy of a small list; render thread only).
    sm::CopyRows(g_rows);
    if (g_selected >= static_cast<int>(g_rows.size())) g_selected = -1;

    const ImGuiIO& io = ImGui::GetIO();

    // Modal backdrop: dim the whole screen behind the browser so the menu behind reads
    // "slightly darker" the moment MULTIPLAYER is pressed (and restores when the browser
    // closes). This is the coop analogue of the native menu's darken-on-navigate feel --
    // VOTV's native darken is the loadLevel transition fade (fade/fadein + SetRenderOpacity),
    // which opening a browser has no equivalent for, so we supply the dim ourselves. Drawn
    // on the BACKGROUND draw list: it sits UNDER this window but OVER the game/menu.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, 80));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(780.0f), S(480.0f)), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.0f), S(14.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.97f));

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin("MULTIPLAYER###coop_browser", &open, flags);
    // The title-bar X clears `open` DURING Begin(), before the body runs -- so if `open`
    // is already false here, the X was clicked (the in-body "Close" button plays its own
    // click later). Give the X the click sound too (click only, no rollover).
    if (!open) ui::menu_sfx::PlayClick();
    if (visible) {
        // Header.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.00f, 1.0f));
        ImGui::TextUnformatted("Server browser");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sm::DisplayVersion().c_str());
        ImGui::Spacing();

        // Your display name -- sent on Join + shown on your nameplate/scoreboard. Applies
        // to the NEXT Host/Join (persisted in session_manager; wins over the config default).
        ImGui::TextUnformatted("Your name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(220.0f));
        if (ImGui::InputText("##nick", g_nick, sizeof(g_nick))) sm::SetNickname(g_nick);
        // Persist the name once the user finishes editing (not per-keystroke) so it
        // sticks across relaunches via multivoid.ini's net.nick (the same key boot reads).
        if (ImGui::IsItemDeactivatedAfterEdit()) coop::config::WriteIniValue("net.nick", g_nick);
        ImGui::Spacing();

        // Host controls row.
        ImGui::SetNextItemWidth(S(220.0f));
        ImGui::InputText("##hostname", g_hostName, sizeof(g_hostName));
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &g_hostLocked);
        ImGui::SameLine();
        if (ui::menu_sfx::Button("Host Game")) {
            // Open the save picker (native loadSlots list + New Game); on confirm it
            // calls HostWithSave -> the harness loads the chosen world then hosts. (The
            // old immediate HostLobby-on-the-current-world path is gone, RULE 2.)
            ui::host_save_picker::Open(g_hostName, g_hostLocked, /*playersMax=*/4);
        }
        ImGui::SameLine(0.0f, S(24.0f));
        if (ui::menu_sfx::Button("Refresh")) sm::Refresh();

        // Direct-connect row (rung 0 -- works even with the master down).
        ImGui::TextDisabled("Direct connect:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(220.0f));
        // Enter in the address field = Connect (the chat-input lesson 2026-07-04).
        const bool ipEnter = ImGui::InputText("##directip", g_directIp, sizeof(g_directIp),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        // Remember the typed address across relaunches (multivoid.ini browser.lastdirect).
        if (ImGui::IsItemDeactivatedAfterEdit()) coop::config::WriteIniValue("browser.lastdirect", g_directIp);
        ImGui::SameLine();
        // Close the browser only if the action was ACCEPTED (good address, not busy) so a
        // rejected click leaves the browser open instead of stranding the user on a clean
        // menu. The loading screen (raised inside ConnectDirect) takes over. (regression B.)
        if (ui::menu_sfx::Button("Connect##direct") || ipEnter) { if (sm::ConnectDirect(g_directIp)) Close(); }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Our own announced lobby: SHOWN in the list (so the host can confirm their
        // server is live + visible to clients) but tagged "(your server)" and gated from
        // self-connect -- you can see it, you just can't join yourself.
        const std::string ownLobby = sm::OwnLobbyId();

        // Server table (MTA column model; "Age" replaces MTA's pre-listed ping).
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##serverlist", 6, tflags, ImVec2(0.0f, -footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, S(22.0f));   // lock
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, S(70.0f));
            ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed, S(54.0f));
            ImGui::TableSetupColumn("World",   ImGuiTableColumnFlags_WidthFixed, S(140.0f));
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, S(96.0f));
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) {
                const Row& r = g_rows[i];
                const bool isOwn = !ownLobby.empty() && r.lobbyId == ownLobby;
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                if (isOwn) ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1.0f), "*");       // your server
                else if (r.locked) ImGui::TextColored(ImVec4(1.00f, 0.78f, 0.35f, 1.0f), "L");

                ImGui::TableSetColumnIndex(1);
                // Whole row is selectable (spans columns); double-click connects -- EXCEPT
                // your OWN server (you host it: visible here so you know it's up, but not joinable).
                const std::string label = isOwn ? (r.name + "   (your server)") : r.name;
                if (isOwn) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.92f, 0.70f, 1.0f));
                if (ImGui::Selectable(label.c_str(), g_selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (isOwn) sm::SetHostStatus("That's your own server -- you're hosting it.");
                        else if (sm::JoinLobby(r.lobbyId, r.name, r.proto, r.game)) Close();
                    }
                }
                if (isOwn) ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d/%d", r.playersCur, r.playersMax);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%ds", r.ageSec);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(r.world.c_str());
                ImGui::TableSetColumnIndex(5);
                // Version column = the host's PAIR identity "0.9.0-n b122" (game
                // target + build number, user 2026-07-19 -- the build number shows
                // everywhere). Amber + (!) when EITHER axis mismatches ours -- the
                // same tiers the Join gate refuses on, so amber ALWAYS means "join
                // will be refused with a popup". Unknown fields (pre-field host)
                // skip their axis; game "" falls back to the legacy version tag
                // (old hosts announced their game version there).
                const bool gameMismatch =
                    !r.game.empty() && r.game != sm::GameTarget();
                const bool verMismatch =
                    gameMismatch ||
                    (r.proto > 0 &&
                     r.proto != static_cast<int>(coop::net::kProtocolVersion));
                std::string verCell = !r.game.empty() ? r.game : r.version;
                if (r.proto > 0) verCell += " b" + std::to_string(r.proto);
                if (verMismatch)
                    ImGui::TextColored(ImVec4(1.00f, 0.78f, 0.35f, 1.0f), "%s (!)",
                                       verCell.c_str());
                else
                    ImGui::TextDisabled("%s", verCell.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Footer: selection-aware Connect (join the selected master lobby) + status.
        // The host's OWN selected server shows a disabled "Your server" instead of Connect.
        const bool hasSel = g_selected >= 0 && g_selected < static_cast<int>(g_rows.size());
        const bool selOwn = hasSel && !ownLobby.empty() && g_rows[g_selected].lobbyId == ownLobby;
        const bool canConnect = hasSel && !selOwn;
        if (!canConnect) ImGui::BeginDisabled();
        if (ui::menu_sfx::Button(selOwn ? "Your server" : "Connect", ImVec2(S(120.0f), 0.0f)) && canConnect)
            if (sm::JoinLobby(g_rows[g_selected].lobbyId, g_rows[g_selected].name,
                              g_rows[g_selected].proto, g_rows[g_selected].game)) Close();
        if (!canConnect) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::menu_sfx::Button("Close", ImVec2(S(90.0f), 0.0f))) open = false;
        ImGui::SameLine(0.0f, S(18.0f));
        const std::string status = sm::Status();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), "%s", status.c_str());

        // Host-action result (set by the Host-Game flow). Green = listed, amber =
        // hosting but master unreachable (unlisted), red = hard failure. Empty until
        // a Host action runs. This is the non-silent feedback for "Host did nothing".
        const std::string hostStatus = sm::HostStatus();
        if (!hostStatus.empty()) {
            const bool failed = hostStatus.rfind("Host failed", 0) == 0;
            const bool unlisted = hostStatus.find("not listed") != std::string::npos ||
                                  hostStatus.find("NOT listed") != std::string::npos;
            const ImVec4 col = failed   ? ImVec4(1.00f, 0.45f, 0.40f, 1.0f)
                              : unlisted ? ImVec4(1.00f, 0.80f, 0.35f, 1.0f)
                                         : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
            ImGui::TextColored(col, "Host: %s", hostStatus.c_str());
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ui::menu_sfx::FrameEnd();  // re-arm rollover if the pointer left all sfx buttons

    if (!open) Close();  // title-bar X or the Close button
}

}  // namespace ui::server_browser
