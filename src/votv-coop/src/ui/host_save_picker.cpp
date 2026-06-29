// ui/host_save_picker.cpp -- see ui/host_save_picker.h.

#include "ui/host_save_picker.h"

#include "ui/server_browser.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"
#include "ue_wrap/log.h"
#include "ue_wrap/save_browser.h"

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::host_save_picker {
namespace {

namespace sm = coop::session_manager;
namespace sb = ue_wrap::save_browser;

std::atomic<bool> g_open{false};

// Render-thread-only UI state.
std::vector<sb::SaveInfo> g_saves;
int  g_selected = -1;
char g_newName[64] = "";
// Lobby host params captured from the server browser on Open().
std::string g_hostName = "My VOTV Server";
bool g_hostLocked = false;
int  g_hostMax = 4;
// Connection selector (user 2026-06-11): false = AUTO (recommended; P2P/ICE --
// direct when the NAT allows it, relay automatically otherwise; nothing to
// configure), true = DIRECT (LanDirect UDP listen; needs the port forwarded;
// friends join via Direct Connect or the browser). AUTO is ALWAYS listed -- the
// master is a relay game's only rendezvous, so a hidden AUTO game is unjoinable
// (hide it in-game via the scoreboard once friends are in). g_hideDirect is the
// DIRECT-only "don't list me" toggle (friends still Direct Connect by IP).
bool g_direct = false;
bool g_hideDirect = false;

// Slot/display names are ASCII; downconvert wide->narrow for ImGui (non-ASCII -> '?').
std::string W2A(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

void DoHostExisting(const sb::SaveInfo& info) {
    sm::SaveChoice c;
    c.newGame = false;
    c.slot = W2A(info.slot);
    const std::string label = W2A(info.displayName.empty() ? info.slot : info.displayName);
    // Raise the host-boot cover only if the host action is ACCEPTED -- it hides the menu
    // so the user can't wander into the browser and connect to their own lobby while the
    // world loads, and gives "Starting your server -- loading <world>" feedback.
    // DriveHostBootIfPending Reset()s it on session-start/failure; if HostWithSave is
    // rejected (busy) there is no pending boot to Reset, so we must NOT raise it.
    if (!sm::HostWithSave(c, g_hostName, g_hostLocked, g_hostMax,
                          g_direct, /*hideFromBrowser=*/g_direct && g_hideDirect)) {
        UE_LOGW("host_save_picker: HOST existing '%s' rejected (busy) -- leaving picker open", c.slot.c_str());
        return;
    }
    coop::join_progress::BeginHostBoot(label);
    UE_LOGI("host_save_picker: HOST existing save '%s'", c.slot.c_str());
    Close();
    ui::server_browser::Close();
}

void DoHostNew() {
    if (g_newName[0] == '\0') return;
    sm::SaveChoice c;
    c.newGame = true;
    c.newName = g_newName;
    c.mode = 0;  // Story (the coop target). Sandbox/Infinite NEW games need the native
                 // map picker (cbox_sboxLevel) -- a later increment; for those, host an
                 // EXISTING save from the list (any mode lists + hosts fine).
    // A brand-new game's load is the SLOWEST host boot (the save isn't loadable for tens
    // of seconds), so the no-feedback window was the worst here -- this is exactly where
    // the user self-joined. Cover the menu the instant the action is accepted.
    if (!sm::HostWithSave(c, g_hostName, g_hostLocked, g_hostMax,
                          g_direct, /*hideFromBrowser=*/g_direct && g_hideDirect)) {
        UE_LOGW("host_save_picker: HOST NEW '%s' rejected (busy) -- leaving picker open", g_newName);
        return;
    }
    coop::join_progress::BeginHostBoot(g_newName);
    UE_LOGI("host_save_picker: HOST NEW story game '%s'", g_newName);
    Close();
    ui::server_browser::Close();
}

}  // namespace

void Open(const std::string& hostName, bool locked, int playersMax) {
    g_hostName = hostName.empty() ? std::string("My VOTV Server") : hostName;
    g_hostLocked = locked;
    g_hostMax = playersMax > 0 ? playersMax : 4;
    g_selected = -1;
    g_open.store(true, std::memory_order_relaxed);
    sb::RefreshAsync();  // on-open scan (per the RefreshAsync contract: not per-frame)
}

void Close() { g_open.store(false, std::memory_order_relaxed); }
bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void Render() {
    if (!IsOpen()) return;
    sb::CopySaves(g_saves);  // cheap copy of the cached list (render thread)
    if (g_selected >= static_cast<int>(g_saves.size())) g_selected = -1;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.98f));

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Host Game  -  choose a world###coop_save_picker", &open, flags)) {
        ImGui::TextUnformatted("Pick a save to host, or start a New Game:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) sb::RefreshAsync();
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sb::Status().c_str());
        ImGui::Spacing();

        // The save list (native metadata read off each UsaveSlot_C).
        const float footer = ImGui::GetFrameHeightWithSpacing() * 3.4f;
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##savelist", 5, tflags, ImVec2(0.0f, -footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Mode",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Day",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Health",  ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(g_saves.size()); ++i) {
                const sb::SaveInfo& s = g_saves[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                const std::string disp = W2A(s.displayName.empty() ? s.slot : s.displayName);
                if (ImGui::Selectable(disp.c_str(), g_selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) DoHostExisting(s);
                }
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(W2A(s.modeLabel).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%d", s.day);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f/%.0f", s.health, s.maxHealth);
                ImGui::TableSetColumnIndex(4); ImGui::TextDisabled("%s", W2A(s.version).c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // New Game (Story) row.
        ImGui::Separator();
        ImGui::TextUnformatted("New Game (Story):");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##newname", "save name", g_newName, sizeof(g_newName));
        ImGui::SameLine();
        const bool canNew = g_newName[0] != '\0';
        if (!canNew) ImGui::BeginDisabled();
        if (ImGui::Button("New Game & Host")) DoHostNew();
        if (!canNew) ImGui::EndDisabled();

        // Connection type + visibility (user 2026-06-11; WP10 plain labels,
        // technical depth in the hint lines). TURN is deliberately NOT a
        // choice: it is the automatic fallback inside AUTO's ICE.
        ImGui::Separator();
        ImGui::TextUnformatted("Connection:");
        ImGui::SameLine();
        if (ImGui::RadioButton("AUTO (recommended)", !g_direct)) g_direct = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("DIRECT (port forward)", g_direct)) g_direct = true;
        if (g_direct) {
            ImGui::TextDisabled("Requires UDP port 47621 forwarded to this PC. Friends join from");
            ImGui::TextDisabled("the server browser or Direct Connect. Not sure? Use AUTO.");
            ImGui::Checkbox("Hide from server browser (friends Direct Connect by IP)", &g_hideDirect);
        } else {
            ImGui::TextDisabled("Connects directly when your network allows it, relays automatically");
            ImGui::TextDisabled("otherwise. Works without any router setup. Always listed -- hide it");
            ImGui::TextDisabled("in-game from the scoreboard once your friends have joined.");
        }

        // Footer: Host the selected save / Cancel + the lobby params.
        ImGui::Separator();
        const bool hasSel = g_selected >= 0 && g_selected < static_cast<int>(g_saves.size());
        if (!hasSel) ImGui::BeginDisabled();
        if (ImGui::Button("Host selected save", ImVec2(170.0f, 0.0f)) && hasSel)
            DoHostExisting(g_saves[g_selected]);
        if (!hasSel) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) open = false;
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), "Lobby: %s%s  (max %d)",
                           g_hostName.c_str(), g_hostLocked ? "  [locked]" : "", g_hostMax);
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (!open) Close();
}

}  // namespace ui::host_save_picker
