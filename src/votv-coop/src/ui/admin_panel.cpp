// ui/admin_panel.cpp -- see ui/admin_panel.h.

#include "ui/admin_panel.h"

#include "coop/player/roster.h"
#include "coop/moderation/ban_list.h"
#include "coop/moderation/moderation.h"
#include "coop/moderation/seen_players.h"
#include "ui/link_format.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace ui::admin_panel {
namespace {

using ui::scale::S;

// Snapshots refreshed at ~2 Hz while the pane is open (render thread; the
// snapshot getters are any-thread, but re-copying vectors every frame is
// pointless churn for file-backed lists that change on admin clicks).
double                                    g_lastRefresh = -1.0;
std::vector<coop::seen_players::Entry>    g_seen;
std::vector<coop::ban_list::Entry>        g_bans;

// Pending ban confirmation (render-thread only). Exactly one of slot / guid is
// set: slot >= 1 = an ONLINE peer ban, guid[0] != 0 = an OFFLINE record ban.
//
// g_banToken is the load-bearing capture for the ONLINE case. The modal takes a
// typed reason, so it stays open for as long as the admin types, and peer slots
// recycle -- targeting the slot alone let a permanent IP ban land on the
// successor. The token names the PERSON and is validated against the live
// net-layer authority when the action finally runs.
coop::moderation::PlayerToken g_banToken{};
int  g_banSlot = -1;
char g_banGuid[33] = {};
char g_banNick[24] = {};
char g_banReason[96] = {};

void RefreshSnapshots() {
    const double now = ImGui::GetTime();
    if (g_lastRefresh >= 0.0 && now - g_lastRefresh < 0.5) return;
    g_lastRefresh = now;
    coop::seen_players::GetSnapshot(g_seen);
    coop::ban_list::GetSnapshot(g_bans);
}

void FormatUnix(long long unixTime, char* out, size_t outLen) {
    if (unixTime <= 0) { std::snprintf(out, outLen, "--"); return; }
    const time_t t = static_cast<time_t>(unixTime);
    tm local{};
    if (localtime_s(&local, &t) != 0) { std::snprintf(out, outLen, "--"); return; }
    std::snprintf(out, outLen, "%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
}

void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.82f, 1.00f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void OpenBanFor(int slot, const char* guid, const char* nick,
                coop::moderation::PlayerToken token = {}) {
    g_banToken = token;
    g_banSlot = slot;
    std::snprintf(g_banGuid, sizeof(g_banGuid), "%s", guid ? guid : "");
    std::snprintf(g_banNick, sizeof(g_banNick), "%s", nick ? nick : "");
    g_banReason[0] = '\0';
}

void RenderOnlineSection(const coop::roster::Snapshot& rs) {
    SectionHeader("Online");
    int shown = 0;
    if (ImGui::BeginTable("##admin_online", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthFixed, S(72.f));
        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, S(52.f));
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, S(220.f));
        ImGui::TableHeadersRow();  // declared but never drawn until v131
        for (int i = 0; i < rs.count; ++i) {
            const coop::roster::Row& r = rs.rows[i];
            if (r.isLocal || !r.connected || r.slot < 1) continue;
            ++shown;
            ImGui::TableNextRow();
            ImGui::PushID(r.slot);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.nick[0] ? r.nick : "Remote player");
            // Same renderer as the scoreboard (ui::link_format) -- this panel
            // used to carry its own copy of the cascade, so the vocabulary could
            // drift one surface at a time.
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", ui::link_format::LinkLabel(r.linkKind));
            ImGui::TableSetColumnIndex(2);
            {
                char pb[16];
                ui::link_format::FormatPing(r.ping, r.linkKind, pb, sizeof(pb));
                ImGui::TextDisabled("%s", pb);
            }
            ImGui::TableSetColumnIndex(3);
            // Token, not slot -- see g_banToken. Cheap and uniform: every
            // slot-addressed action captures the person, so no call site has to
            // remember which ones are destructive.
            const auto token = coop::moderation::TokenFor(r.slot, r.playerNo, r.generation);
            if (ImGui::SmallButton("Teleport")) coop::moderation::TeleportPlayerToMe(token);
            ImGui::SameLine();
            if (ImGui::SmallButton("Kick")) coop::moderation::KickPlayer(token);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
            if (ImGui::SmallButton("Ban...")) OpenBanFor(r.slot, nullptr, r.nick, token);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0) ImGui::TextDisabled("No remote players connected.");
}

void RenderOfflineSection() {
    SectionHeader("Offline (seen before)");
    int shown = 0;
    if (ImGui::BeginTable("##admin_offline", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, S(130.f));
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, S(90.f));
        ImGui::TableHeadersRow();
        for (const auto& e : g_seen) {
            if (e.online) continue;  // online rows live in the section above
            ++shown;
            ImGui::TableNextRow();
            ImGui::PushID(e.guid);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(e.nick[0] ? e.nick : "(no nick)");
            ImGui::TableSetColumnIndex(1);
            char when[24];
            FormatUnix(e.lastSeenUnix, when, sizeof(when));
            ImGui::TextDisabled("%s", when);
            ImGui::TableSetColumnIndex(2);
            const bool alreadyBanned = [&] {
                for (const auto& b : g_bans)
                    if (e.ip[0] && std::strcmp(b.ip, e.ip) == 0) return true;
                return false;
            }();
            if (alreadyBanned) {
                ImGui::TextDisabled("banned");
            } else if (!e.ip[0]) {
                ImGui::TextDisabled("no IP");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("No stored IP for this player (P2P join) --\n"
                                      "nothing the IP ban filter could enforce against.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
                if (ImGui::SmallButton("Ban...")) OpenBanFor(-1, e.guid, e.nick);
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0) ImGui::TextDisabled("Nobody in the registry yet (players are recorded when they join).");
}

void RenderBannedSection() {
    SectionHeader("Banned");
    if (ImGui::BeginTable("##admin_banned", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, S(110.f));
        ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, S(130.f));
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, S(70.f));
        ImGui::TableHeadersRow();
        for (const auto& b : g_bans) {
            ImGui::TableNextRow();
            ImGui::PushID(b.ip);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(b.nick[0] ? b.nick : "(no nick)");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", b.ip);
            ImGui::TableSetColumnIndex(2);
            char when[24];
            FormatUnix(b.bannedUnix, when, sizeof(when));
            ImGui::TextDisabled("%s", when);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", b.reason[0] ? b.reason : "--");
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Unban")) {
                coop::moderation::Unban(b.ip);
                g_lastRefresh = -1.0;  // reflect immediately
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (g_bans.empty()) ImGui::TextDisabled("No banned players.");
}

void RenderBanModal() {
    const bool pending = (g_banSlot >= 1) || g_banGuid[0];
    if (pending && !ImGui::IsPopupOpen("Ban player##admin"))
        ImGui::OpenPopup("Ban player##admin");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Ban player##admin", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Permanently ban %s?", g_banNick[0] ? g_banNick : "this player");
        ImGui::TextDisabled(g_banSlot >= 1
                                ? "Disconnected now and blocked by IP on reconnect."
                                : "Blocked by their last known IP on reconnect.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(320.f));
        ImGui::InputTextWithHint("##banreason", "reason (shown in the Banned list)",
                                 g_banReason, sizeof(g_banReason));
        ImGui::Spacing();
        if (ImGui::Button("Ban", ImVec2(S(110.f), 0))) {
            const char* reason = g_banReason[0] ? g_banReason : "banned by host";
            if (g_banSlot >= 1) coop::moderation::BanPlayer(g_banToken, reason);
            else                coop::moderation::BanOffline(g_banGuid, reason);
            g_banSlot = -1;
            g_banGuid[0] = '\0';
            g_lastRefresh = -1.0;  // pull the new ban row promptly
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(110.f), 0))) {
            g_banSlot = -1;
            g_banGuid[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace

void Render() {
    RefreshSnapshots();

    coop::roster::Snapshot rs;
    coop::roster::GetSnapshot(rs);

    ImGui::TextDisabled("Host administration. Bans are permanent (by IP) and survive restarts;");
    ImGui::TextDisabled("the registry remembers every player who ever joined this host.");

    RenderOnlineSection(rs);
    RenderOfflineSection();
    RenderBannedSection();
    RenderBanModal();
}

}  // namespace ui::admin_panel
