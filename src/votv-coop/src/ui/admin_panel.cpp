// ui/admin_panel.cpp -- see ui/admin_panel.h.

#include "ui/admin_panel.h"

#include "l10n/l10n.h"

#include "coop/commands/command_sync.h"
#include "coop/player/roster.h"
#include "coop/moderation/ban_list.h"
#include "coop/moderation/seen_players.h"
#include "ui/link_format.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ui::admin_panel {
namespace {

using ui::scale::S;

// Snapshots refreshed at ~2 Hz while the pane is open (render thread; the
// snapshot getters are any-thread, but re-copying vectors every frame is
// pointless churn for file-backed lists that change on admin clicks).
double                                    g_lastRefresh = -1.0;
std::vector<coop::seen_players::Entry>    g_seen;
// The records of g_seen that are not seated, rebuilt with it: the Offline list's clipper runs over
// this vector, so every index it hands out draws exactly one row.
std::vector<coop::seen_players::Entry>    g_offline;
std::vector<coop::ban_list::Entry>        g_bans;
// The ids in g_bans, rebuilt with it so a row asks "already banned?" with one lookup. The views
// point into g_bans and live exactly as long as that snapshot.
std::unordered_set<std::string_view>      g_banIds;

// Pending ban confirmation (render-thread only): g_banId[0] != 0 means one awaits its confirm.
//
// The proved player id is the load-bearing capture. The modal takes a typed reason, so it stays
// open for as long as the admin types, and peer slots recycle -- targeting the slot alone let a
// permanent ban land on the successor. The id names the PERSON, seated or not; the ban command
// resolves it when the line runs, and bans nobody if no one holds it.
char g_banId[33] = {};
bool g_banOnline = false;  // the person was seated when the modal opened (for its wording)
char g_banNick[coop::text::kNickBufBytes] = {};
char g_banReason[96] = {};
// The modal's "Also refuse their address" box; set again each time the modal opens.
bool g_banByAddress = true;

void RefreshSnapshots() {
    const double now = ImGui::GetTime();
    if (g_lastRefresh >= 0.0 && now - g_lastRefresh < 0.5) return;
    g_lastRefresh = now;
    coop::seen_players::GetSnapshot(g_seen);
    g_offline.clear();
    for (const auto& e : g_seen) {
        if (!e.online) g_offline.push_back(e);  // online rows live in the section above
    }
    coop::ban_list::GetSnapshot(g_bans);
    g_banIds.clear();
    for (const auto& b : g_bans) g_banIds.insert(std::string_view(b.id));
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

void OpenBanFor(bool online, const char* id, const char* nick) {
    g_banOnline = online;
    std::snprintf(g_banId, sizeof(g_banId), "%s", id);
    std::snprintf(g_banNick, sizeof(g_banNick), "%s", nick ? nick : "");
    g_banReason[0] = '\0';
    g_banByAddress = true;
}

// A player's id on hover over its nick: how the host finds the name a permission file is stored
// under.
void IdTooltip(const char* id) {
    if (id[0] == '\0' || !ImGui::IsItemHovered()) return;
    char tip[256];
    l10n::Fmt(tip, sizeof(tip), l10n::T("Player id %s"), id);
    ImGui::SetTooltip("%s", tip);
}

// A small button that is off, with the reason on hover, while the person has no proved id yet.
bool IdButton(const char* label, bool hasId) {
    if (!hasId) ImGui::BeginDisabled();
    const bool clicked = ImGui::SmallButton(label);
    if (!hasId) ImGui::EndDisabled();
    if (!hasId && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", l10n::T("Their identity is not proved yet."));
    return clicked;
}

void RenderOnlineSection(const coop::roster::Snapshot& rs) {
    SectionHeader(l10n::T("Online"));
    int shown = 0;
    if (ImGui::BeginTable("##admin_online", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Player"), "player"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Link"), "link"), ImGuiTableColumnFlags_WidthFixed, S(72.f));
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Ping"), "ping"), ImGuiTableColumnFlags_WidthFixed, S(52.f));
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Actions"), "actions"), ImGuiTableColumnFlags_WidthFixed, S(220.f));
        ImGui::TableHeadersRow();
        for (int i = 0; i < rs.count; ++i) {
            const coop::roster::Row& r = rs.rows[i];
            if (r.isLocal || !r.connected || r.slot < 1) continue;
            ++shown;
            ImGui::TableNextRow();
            ImGui::PushID(r.slot);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.nick[0] ? r.nick : l10n::T("Remote player"));
            IdTooltip(r.playerId);
            if (r.otherBuild) {
                ImGui::SameLine(0.0f, S(6.0f));
                ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.35f, 0.85f), "%s", l10n::T("other build"));
            }
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
            // Each button submits a command line for the person's proved id, not their slot --
            // see g_banId. Without a proved id the buttons are off.
            const bool hasId = r.playerId[0] != '\0';
            if (IdButton(l10n::Label(l10n::Tc("f1", "Teleport"), "teleport"), hasId))
                coop::command_sync::Submit(std::string("tphere ") + r.playerId);
            ImGui::SameLine();
            if (IdButton(l10n::Label(l10n::Tc("f1", "Kick"), "kick"), hasId))
                coop::command_sync::Submit(std::string("kick ") + r.playerId);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
            if (IdButton(l10n::Label(l10n::Tc("f1", "Ban..."), "ban_"), hasId)) OpenBanFor(true, r.playerId, r.nick);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0) ImGui::TextDisabled("%s", l10n::T("No remote players connected."));
}

void RenderOfflineSection() {
    SectionHeader(l10n::T("Offline (seen before)"));
    if (ImGui::BeginTable("##admin_offline", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Player"), "player"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Last seen"), "last_seen"), ImGuiTableColumnFlags_WidthFixed, S(130.f));
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Actions"), "actions"), ImGuiTableColumnFlags_WidthFixed, S(90.f));
        ImGui::TableHeadersRow();
        // Only the rows in view are drawn: the registry grows with every player who ever joined.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_offline.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& e = g_offline[static_cast<size_t>(i)];
                ImGui::TableNextRow();
                ImGui::PushID(e.guid);
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(e.nick[0] ? e.nick : l10n::T("(no nick)"));
                IdTooltip(e.guid);
                ImGui::TableSetColumnIndex(1);
                char when[24];
                FormatUnix(e.lastSeenUnix, when, sizeof(when));
                ImGui::TextDisabled("%s", when);
                ImGui::TableSetColumnIndex(2);
                const bool alreadyBanned = g_banIds.count(std::string_view(e.guid)) != 0;
                if (alreadyBanned) {
                    ImGui::TextDisabled("%s", l10n::T("banned"));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
                    if (ImGui::SmallButton(l10n::Label(l10n::Tc("f1", "Ban..."), "ban_"))) OpenBanFor(false, e.guid, e.nick);
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (g_seen.empty()) ImGui::TextDisabled("%s", l10n::T("Nobody in the registry yet (players are recorded when they join)."));
    else if (g_offline.empty()) ImGui::TextDisabled("%s", l10n::T("Everyone in the registry is online."));
}

void RenderBannedSection() {
    SectionHeader(l10n::T("Banned"));
    if (coop::ban_list::IsReadOnly())
        ImGui::TextDisabled("%s", l10n::T("The ban file has entries this version cannot read, so bans made now last until the "
                                          "server stops. See the log."));
    if (ImGui::BeginTable("##admin_banned", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Player"), "player"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(l10n::Label(l10n::T("ID"), "id"), ImGuiTableColumnFlags_WidthFixed, S(110.f));
        ImGui::TableSetupColumn(l10n::Label(l10n::T("When"), "when"), ImGuiTableColumnFlags_WidthFixed, S(130.f));
        ImGui::TableSetupColumn(l10n::Label(l10n::T("Reason"), "reason"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, S(70.f));
        ImGui::TableHeadersRow();
        // Only the rows in view are drawn: a client granted the offline ban can grow this list.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_bans.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& b = g_bans[static_cast<size_t>(i)];
                ImGui::TableNextRow();
                ImGui::PushID(b.id);
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(b.nick[0] ? b.nick : l10n::T("(no nick)"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%.8s", b.id);
                if (ImGui::IsItemHovered()) {
                    char tip[256];
                    l10n::Fmt(tip, sizeof(tip), l10n::T("Player id %1$s\nAddress %2$s"), b.id,
                              b.address[0] ? b.address : l10n::T("not blocked"));
                    ImGui::SetTooltip("%s", tip);
                }
                ImGui::TableSetColumnIndex(2);
                char when[24];
                FormatUnix(b.bannedUnix, when, sizeof(when));
                ImGui::TextDisabled("%s", when);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s", b.reason[0] ? b.reason : "--");
                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton(l10n::Label(l10n::Tc("f1", "Unban"), "unban"))) {
                    coop::command_sync::Submit(std::string("unban ") + b.id);
                    g_lastRefresh = -1.0;  // the row drops at the next refresh after the posted command ran
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (g_bans.empty()) ImGui::TextDisabled("%s", l10n::T("No banned players."));
}

void RenderBanModal() {
    const bool pending = g_banId[0] != '\0';
    // One title for the open, the test and the modal: the text is translated, the id never is.
    const l10n::Label title(l10n::T("Ban player"), "admin");
    if (pending && !ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        char heading[256];
        l10n::Fmt(heading, sizeof(heading), l10n::T("Permanently ban %s?"),
                  g_banNick[0] ? g_banNick : l10n::T("this player"));
        ImGui::TextUnformatted(heading);
        ImGui::TextDisabled("%s", g_banOnline
                                      ? l10n::T("Disconnected now and refused whenever they rejoin.")
                                      : l10n::T("Refused whenever they rejoin."));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(320.f));
        ImGui::InputTextWithHint("##banreason", l10n::T("reason (shown in the Banned list)"),
                                 g_banReason, sizeof(g_banReason));
        ImGui::Checkbox(l10n::Label(l10n::T("Also refuse their address"), "also_refuse_their_address"),
                        &g_banByAddress);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", l10n::T("Also refuses anyone connecting from the same address -- people who share "
                                            "their router or their provider's address are refused too. It only works on "
                                            "a direct connection: a player who comes through a relay is refused by their "
                                            "identity alone."));
        ImGui::Spacing();
        if (ImGui::Button(l10n::Label(l10n::Tc("f1", "Ban"), "ban"), ImVec2(S(110.f), 0))) {
            // The reason is the raw remainder of the line, never quoted; none typed, the ban
            // command stores "banned by host".
            std::string line = std::string(g_banByAddress ? "ban " : "banid ") + g_banId;
            if (g_banReason[0]) line += std::string(" ") + g_banReason;
            coop::command_sync::Submit(std::move(line));
            g_banId[0] = '\0';
            g_lastRefresh = -1.0;  // pull the new ban row promptly
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(l10n::Label(l10n::Tc("f1", "Cancel"), "cancel"), ImVec2(S(110.f), 0))) {
            g_banId[0] = '\0';
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

    ImGui::TextDisabled("%s", l10n::T("Host administration. Bans follow the player and survive restarts;\n"
                                      "the registry remembers every player who ever joined this host."));

    RenderOnlineSection(rs);
    RenderOfflineSection();
    RenderBannedSection();
    RenderBanModal();
}

}  // namespace ui::admin_panel
