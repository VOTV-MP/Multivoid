// ui/scoreboard.cpp -- see ui/scoreboard.h.

#include "ui/scoreboard.h"

#include "coop/moderation.h"
#include "coop/session_manager.h"  // ListedState / SetListed (the host hide toggle)
#include "coop/roster.h"
#include "coop/voice/voice_chat.h"
#include "ui/dev_menu.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace ui::scoreboard {
namespace {

// Pending permanent-ban confirmation (render-thread only). >=0 == a ban is
// awaiting the modal's confirm; the nick is copied for the prompt text so the
// modal survives the row's roster snapshot changing under it.
int  g_banConfirmSlot = -1;
char g_banConfirmNick[24] = {};

// A small filled status dot drawn inline before a name (green = connected). Uses
// the window draw list + a Dummy spacer so the following SameLine() name lands
// just to its right, vertically centred on the text line.
void StatusDot(bool connected) {
    const ImVec4 col = connected ? ImVec4(0.36f, 0.85f, 0.42f, 1.0f)
                                 : ImVec4(0.60f, 0.60f, 0.62f, 1.0f);
    const float radius = 4.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + radius, p.y + ImGui::GetTextLineHeight() * 0.5f),
        radius, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
}

}  // namespace

bool LocalIsHost() { return coop::roster::LocalIsHost(); }  // lock-free (overlay hot path)

void Render() {
    coop::roster::Snapshot s;
    coop::roster::GetSnapshot(s);

    const ImGuiIO& io = ImGui::GetIO();
    // Top-centre, pinned. Pivot (0.5, 0) keeps it centred regardless of width.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.14f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));  // upper-centre, below the top HUD
    // 460 wide (was 300 -- truncated nicks + "LAN HOST" clipped, user round-1
    // screenshot 2026-06-12); auto height.
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);

    // Clean translucent panel: padded, rounded, borderless, dark bg so it reads over
    // any scene. Own header (no OS-style title bar).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 13.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.94f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

    coop::voice_chat::UiSnapshot vs;
    coop::voice_chat::GetUiSnapshot(vs);

    if (ImGui::Begin("###coop_scoreboard", nullptr, flags)) {
        // Header: bright "PLAYERS" + accent online count + a dim "V: voice" key
        // hint (the settings window moved to its own V-key surface, user
        // 2026-06-12 round 1 -- no button here).
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.00f, 1.0f));
        ImGui::TextUnformatted("PLAYERS");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 8.0f);
        if (s.inSession) ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.00f, 1.0f), "%d online", s.count);
        else             ImGui::TextDisabled("offline");
        if (vs.enabled != 0 && vs.started != 0) {
            const float bw = ImGui::CalcTextSize("V: voice settings").x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
            ImGui::TextDisabled("V: voice settings");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Role is conveyed by NICK COLOUR (host = gold, client = soft white) -- no
        // separate role text column. "(you)" marks the local player. On the HOST's
        // interactive board, every OTHER connected client's row is a clickable
        // Selectable that opens an action popup (Kick / Ban always; Teleport-to-me
        // under [dev] devkeys). A client's board (and the host's own row) is plain
        // text -- the client peek is passive (no input capture).
        const bool host = LocalIsHost();
        const bool dev  = ui::dev_menu::DevMode();
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        const bool voiceOn = vs.enabled != 0 && vs.started != 0;
        if (ImGui::BeginTable("##roster", voiceOn ? 4 : 3, tflags)) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
            // Voice state icon (v66; the user's mute-icon-on-playerlist ask).
            // Click = self: toggle mute; remote: per-player volume popup.
            if (voiceOn) ImGui::TableSetupColumn("Mic", ImGuiTableColumnFlags_WidthFixed, 26.0f);
            // Connection type (user 2026-06-10): how the game is hosted on the
            // host row ("LAN HOST"/"P2P HOST") + each link's transport ("LAN"/
            // "P2P"/"P2P RELAY"; "VIA HOST" for peer clients on a client board).
            ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            for (int i = 0; i < s.count; ++i) {
                const coop::roster::Row& r = s.rows[i];
                const char* nick = r.nick[0] ? r.nick : (r.isLocal ? "Player" : "Remote player");
                const ImVec4 nickCol = r.isHost ? ImVec4(1.00f, 0.82f, 0.35f, 1.0f)   // host  = gold
                                                : ImVec4(0.86f, 0.89f, 0.94f, 1.0f);  // client = soft white
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(r.slot);
                StatusDot(r.connected);
                ImGui::SameLine();

                const bool actionable = host && !r.isLocal && r.connected && r.slot >= 1;
                if (actionable) {
                    ImGui::PushStyleColor(ImGuiCol_Text, nickCol);
                    // DontClosePopups: clicking the row toggles the popup; the
                    // selection state itself is meaningless here. AllowOverlap:
                    // the row spans ALL columns, and ImGui gives a click to the
                    // FIRST-submitted item -- without it the row selectable eats
                    // the Mic-column icon's clicks and pops the kick/ban menu
                    // instead of the volume popup (user 2026-06-12 round 2).
                    if (ImGui::Selectable(nick, false,
                                          ImGuiSelectableFlags_DontClosePopups |
                                              ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowOverlap))
                        ImGui::OpenPopup("##act");
                    ImGui::PopStyleColor();

                    if (ImGui::BeginPopup("##act")) {
                        ImGui::TextDisabled("%s", nick);
                        ImGui::Separator();
                        if (dev && ImGui::MenuItem("Teleport to me"))
                            coop::moderation::TeleportSlotToMe(r.slot);
                        if (ImGui::MenuItem("Kick"))
                            coop::moderation::KickSlot(r.slot);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
                        const bool banClicked = ImGui::MenuItem("Ban (permanent)");
                        ImGui::PopStyleColor();
                        if (banClicked) {
                            g_banConfirmSlot = r.slot;
                            std::snprintf(g_banConfirmNick, sizeof(g_banConfirmNick), "%s", nick);
                        }
                        ImGui::EndPopup();
                    }
                } else {
                    ImGui::TextColored(nickCol, "%s", nick);
                    if (r.isLocal) { ImGui::SameLine(0.0f, 6.0f); ImGui::TextDisabled("(you)"); }
                }

                // Voice column (v66): the per-player mic-state icon. Self row:
                // click toggles your mute. Remote row: click opens the local
                // mute/volume popup (SetSlotVolume is render-thread-safe).
                int col = 1;
                if (voiceOn) {
                    ImGui::TableSetColumnIndex(col++);
                    const bool self = r.isLocal;
                    const auto icon = static_cast<coop::voice_chat::VoiceIcon>(
                        r.slot >= 0 && r.slot < static_cast<int>(coop::players::kMaxPeers)
                            ? vs.icons[r.slot] : 0);
                    const float ih = ImGui::GetTextLineHeight();
                    const ImVec2 cell = ImGui::GetCursorScreenPos();
                    // Locally-silenced peers show the muted-mic glyph dimmed even
                    // when their own state is None (you chose not to hear them).
                    const bool localSilenced =
                        !self && r.slot >= 0 &&
                        r.slot < static_cast<int>(coop::players::kMaxPeers) &&
                        vs.slotVolume[r.slot] <= 0.001f;
                    if (ImGui::InvisibleButton("##vicon", ImVec2(20.0f, ih))) {
                        if (self) coop::voice_chat::SetMuted(vs.muted == 0);
                        else if (r.connected) ImGui::OpenPopup("##vvol");
                    }
                    // An idle peer (icon None) draws a DIM mic outline instead of
                    // nothing -- an invisible cell read as "no mute icons" (user
                    // round 1) and gave the click target no affordance.
                    const bool idle =
                        !localSilenced && icon == coop::voice_chat::VoiceIcon::None;
                    const auto shown = localSilenced ? coop::voice_chat::VoiceIcon::MicMuted
                                       : idle        ? coop::voice_chat::VoiceIcon::Talking
                                                     : icon;
                    ui::voice_icons::Draw(ImGui::GetWindowDrawList(),
                                          ImVec2(cell.x + 10.0f, cell.y + ih * 0.5f),
                                          ih * 0.95f, shown,
                                          localSilenced ? 0.55f : idle ? 0.30f : 1.0f);
                    if (ImGui::IsItemHovered()) {
                        const char* lbl = localSilenced ? "Muted for you (click for volume)"
                                          : self ? (vs.muted ? "Your mic is muted -- click to unmute"
                                                             : "Click to mute your mic")
                                          : idle ? "Voice connected (click for volume)"
                                                 : ui::voice_icons::Label(shown);
                        if (lbl && lbl[0]) ImGui::SetTooltip("%s", lbl);
                    }
                    if (!self && ImGui::BeginPopup("##vvol")) {
                        ImGui::TextDisabled("%s", nick);
                        ImGui::Separator();
                        float v = vs.slotVolume[r.slot];
                        bool silenced = v <= 0.001f;
                        if (ImGui::Checkbox("Mute for me", &silenced))
                            coop::voice_chat::SetSlotVolume(r.slot, silenced ? 0.0f : 1.0f);
                        if (!silenced) {
                            if (ImGui::SliderFloat("##pvol", &v, 0.0f, 2.0f, "volume %.2fx"))
                                coop::voice_chat::SetSlotVolume(r.slot, v);
                        }
                        ImGui::EndPopup();
                    }
                }

                // Link column: the connection-type label (empty on the own client
                // row -- its link is already shown on the host row).
                ImGui::TableSetColumnIndex(col++);
                if (r.link[0]) ImGui::TextDisabled("%s", r.link);

                // Ping column: per-peer RTT, right-aligned (remote connected peers only;
                // you have no self-ping). 0 on a sub-ms LAN shows "<1ms"; -1 (unsampled)
                // shows "--".
                ImGui::TableSetColumnIndex(col);
                if (!r.isLocal && r.connected) {
                    char pb[16];
                    if (r.ping > 0)       std::snprintf(pb, sizeof(pb), "%dms", r.ping);
                    else if (r.ping == 0) std::snprintf(pb, sizeof(pb), "<1ms");
                    else                  std::snprintf(pb, sizeof(pb), "--");
                    const float tw = ImGui::CalcTextSize(pb).x;
                    const float cw = ImGui::GetContentRegionAvail().x;
                    if (cw > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw));
                    ImGui::TextDisabled("%s", pb);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (s.count == 0) ImGui::TextDisabled("No players.");

        // Host-only "Hide from server browser" toggle (user 2026-06-11). This is
        // where an AUTO/relay host hides the lobby ONCE friends are in -- hiding at
        // host time would make it unjoinable (the master is the only rendezvous), so
        // it lives here, not in the Host-Game picker. Posts /v1/visibility async;
        // the session stays live. Mirrors the master state via ListedState().
        if (host) {
            ImGui::Separator();
            bool listed = coop::session_manager::ListedState();
            if (ImGui::Checkbox("Show in server browser", &listed))
                coop::session_manager::SetListed(listed);
            ImGui::SameLine();
            ImGui::TextDisabled(listed ? "(others can find your game)"
                                       : "(hidden -- friends join by invite/IP)");
        }

        // Permanent-ban confirmation modal (shared across rows; g_banConfirmSlot
        // carries the pending slot). A ban is destructive + irreversible from the
        // UI, so it gets an explicit confirm step. Opened once on the transition;
        // closed by either button.
        if (g_banConfirmSlot >= 0 && !ImGui::IsPopupOpen("Confirm ban##coop"))
            ImGui::OpenPopup("Confirm ban##coop");
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Confirm ban##coop", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Permanently ban %s?", g_banConfirmNick);
            ImGui::TextDisabled("Disconnected now and blocked by IP on reconnect.");
            ImGui::Spacing();
            if (ImGui::Button("Ban", ImVec2(110, 0))) {
                coop::moderation::BanSlot(g_banConfirmSlot);
                g_banConfirmSlot = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0))) {
                g_banConfirmSlot = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

}  // namespace ui::scoreboard
