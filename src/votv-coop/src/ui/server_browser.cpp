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
#include "coop/session_manager.h"
#include "ue_wrap/log.h"

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

std::atomic<bool> g_open{false};
std::atomic<bool> g_justOpened{false};   // Open() -> Render triggers one auto-refresh

// Render-thread-only UI state.
std::vector<Row> g_rows;
int  g_selected = -1;
char g_directIp[64] = "127.0.0.1:7777";
char g_hostName[64] = "My VOTV Server";
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

    // Auto-refresh once when the browser is opened (so the list isn't stale/empty).
    if (g_justOpened.exchange(false, std::memory_order_relaxed)) {
        sm::Refresh();
    }
    // Pull the latest fetched rows (cheap copy of a small list; render thread only).
    sm::CopyRows(g_rows);
    if (g_selected >= static_cast<int>(g_rows.size())) g_selected = -1;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780.0f, 480.0f), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.97f));

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("MULTIPLAYER###coop_browser", &open, flags)) {
        // Header.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.00f, 1.0f));
        ImGui::TextUnformatted("Server browser");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("(Voices of the Void  -  Coop %s)", sm::ModVersion());
        ImGui::Spacing();

        // Host controls row.
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##hostname", g_hostName, sizeof(g_hostName));
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &g_hostLocked);
        ImGui::SameLine();
        if (ImGui::Button("Host Game")) {
            // Open the save picker (native loadSlots list + New Game); on confirm it
            // calls HostWithSave -> the harness loads the chosen world then hosts. (The
            // old immediate HostLobby-on-the-current-world path is gone, RULE 2.)
            ui::host_save_picker::Open(g_hostName, g_hostLocked, /*playersMax=*/4);
        }
        ImGui::SameLine(0.0f, 24.0f);
        if (ImGui::Button("Refresh")) sm::Refresh();

        // Direct-connect row (rung 0 -- works even with the master down).
        ImGui::TextDisabled("Direct connect:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##directip", g_directIp, sizeof(g_directIp));
        ImGui::SameLine();
        // Close the browser only if the action was ACCEPTED (good address, not busy) so a
        // rejected click leaves the browser open instead of stranding the user on a clean
        // menu. The loading screen (raised inside ConnectDirect) takes over. (regression B.)
        if (ImGui::Button("Connect##direct")) { if (sm::ConnectDirect(g_directIp)) Close(); }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Server table (MTA column model; "Age" replaces MTA's pre-listed ping).
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##serverlist", 6, tflags, ImVec2(0.0f, -footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 22.0f);   // lock
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed, 54.0f);
            ImGui::TableSetupColumn("World",   ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 76.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) {
                const Row& r = g_rows[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                if (r.locked) ImGui::TextColored(ImVec4(1.00f, 0.78f, 0.35f, 1.0f), "L");

                ImGui::TableSetColumnIndex(1);
                // Whole row is selectable (spans columns); double-click connects.
                if (ImGui::Selectable(r.name.c_str(), g_selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        if (sm::JoinLobby(r.lobbyId, r.name)) Close();
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d/%d", r.playersCur, r.playersMax);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%ds", r.ageSec);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(r.world.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextDisabled("%s", r.version.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Footer: selection-aware Connect (join the selected master lobby) + status.
        const bool hasSel = g_selected >= 0 && g_selected < static_cast<int>(g_rows.size());
        if (!hasSel) ImGui::BeginDisabled();
        if (ImGui::Button("Connect", ImVec2(120.0f, 0.0f)) && hasSel)
            if (sm::JoinLobby(g_rows[g_selected].lobbyId, g_rows[g_selected].name)) Close();
        if (!hasSel) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) open = false;
        ImGui::SameLine(0.0f, 18.0f);
        const std::string status = sm::Status();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), "%s", status.c_str());
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (!open) Close();  // title-bar X or the Close button
}

}  // namespace ui::server_browser
