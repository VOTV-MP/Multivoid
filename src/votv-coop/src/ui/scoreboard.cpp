// ui/scoreboard.cpp -- see ui/scoreboard.h.

#include "ui/scoreboard.h"

#include "coop/commands/command_sync.h"
#include "coop/session/session_manager.h"  // ListedState / SetListed -- the ONE mid-session edit
#include "coop/player/nick_color.h"
#include "coop/player/roster.h"
#include "coop/voice/voice_chat.h"
#include "l10n/l10n.h"
#include "ui/link_format.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ui::scoreboard {
namespace {

using ui::scale::S;

// The player an open action popup or ban confirmation names, render thread only. The person is
// copied (proved id and nick) when the popup opens: the popup and the modal sit open across an
// arbitrary delay and slots recycle, so an action aimed at the row's slot, or at whatever the
// row shows by then, could land on whoever inherited the seat. A proved id names the person.
char g_actId[33] = {};
char g_actNick[coop::text::kNickBufBytes] = {};
// The pending ban confirmation: an id awaits the modal's confirm, with the nick for the prompt.
char g_banConfirmId[33] = {};
char g_banConfirmNick[coop::text::kNickBufBytes] = {};
// The modal's "Also refuse their address" box; set again each time the modal opens.
bool g_banConfirmByAddress = true;

// A small filled status dot before a name, green when connected, drawn on the window draw list
// with a dummy spacer so the following name lands to its right, centred on the text line.
void StatusDot(bool connected) {
    const ImVec4 col = connected ? ImVec4(0.36f, 0.85f, 0.42f, 1.0f)
                                 : ImVec4(0.60f, 0.60f, 0.62f, 1.0f);
    const float radius = S(4.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + radius, p.y + ImGui::GetTextLineHeight() * 0.5f),
        radius, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(radius * 2.0f + S(6.0f), ImGui::GetTextLineHeight()));
}

// On the host, a row's nick hovered shows the player id a permission file is named by.
void IdTooltip(const coop::roster::Row& r, bool host) {
    if (!host || r.playerId[0] == '\0' || !ImGui::IsItemHovered()) return;
    char tip[256];
    l10n::Fmt(tip, sizeof(tip), r.isLocal ? l10n::T("Your player id %s") : l10n::T("Player id %s"),
              r.playerId);
    ImGui::SetTooltip("%s", tip);
}

// A menu item that is off, with the reason on hover, while the person has no proved id yet. The
// text is already looked up; `id` is the widget's own, so a translation never moves it.
bool IdMenuItem(const char* text, const char* id, bool hasId) {
    const bool clicked = ImGui::MenuItem(l10n::Label(text, id), nullptr, false, hasId);
    if (!hasId && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", l10n::T("Their identity is not proved yet."));
    return clicked;
}

// The ban modal's popup id as OpenPopup and IsPopupOpen name it: the "###" form of the id the
// modal's Label carries ("coop"), so a translated title still opens and begins one popup.
constexpr const char* kBanModalId = "###coop";

}  // namespace

bool LocalIsHost() { return coop::roster::LocalIsHost(); }  // lock-free (overlay hot path)

void Render() {
    coop::roster::Snapshot s;
    coop::roster::GetSnapshot(s);

    const ImGuiIO& io = ImGui::GetIO();
    // Top-centre, pinned; the pivot keeps it centred regardless of width.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.14f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));  // upper-centre, below the top HUD
    // 540 wide, auto height: wide enough for the column headers to sit over their values, so the
    // strip does not read as one run of words.
    ImGui::SetNextWindowSize(ImVec2(S(540.0f), 0.0f), ImGuiCond_Always);

    // A translucent panel: padded, rounded, borderless, dark, so it reads over any scene; its own
    // header, no title bar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.0f), S(13.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6.0f), S(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.94f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

    coop::voice_chat::UiSnapshot vs;
    coop::voice_chat::GetUiSnapshot(vs);

    if (ImGui::Begin("###coop_scoreboard", nullptr, flags)) {
        // The header: PLAYERS, the online count in the accent, and a dim key hint for the voice
        // settings, which live on their own surface.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.00f, 1.0f));
        ImGui::TextUnformatted(l10n::T("PLAYERS"));
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, S(8.0f));
        if (s.inSession) {
            char online[256];
            l10n::Fmt(online, sizeof(online), l10n::T("%d online"), s.count);
            ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.00f, 1.0f), "%s", online);
        } else {
            ImGui::TextDisabled("%s", l10n::T("offline"));
        }
        if (vs.enabled != 0 && vs.started != 0) {
            const char* voiceHint = l10n::T("V: voice settings");
            const float bw = ImGui::CalcTextSize(voiceHint).x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
            ImGui::TextDisabled("%s", voiceHint);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Role is conveyed by nick colour (host gold, client soft white), with no role column, and
        // "(you)" marks the local player. On the host's board every other connected client's row is
        // a selectable that opens the action popup (teleport to me, kick, ban: host-standard admin
        // verbs, not dev-gated); a client's board and the host's own row are plain text.
        const bool host = LocalIsHost();
        // Inner horizontal borders and a lifted alternate-row tint: with the row background alone
        // the stripes were invisible over the dark overlay and the rows read as one block.
        const ImGuiTableFlags tflags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerH;
        const bool voiceOn = vs.enabled != 0 && vs.started != 0;
        // Over a dark, busy 3D backdrop a stripe has to be far stronger than on a flat UI, so three
        // cues: a visible alternating tint, a real separator line, and vertical room.
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1.0f, 1.0f, 1.0f, 0.28f));
        // Roomier cells: with a header row the table needs vertical air, and the horizontal pad
        // keeps each label off its neighbour's column edge.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8.0f), S(7.0f)));
        if (ImGui::BeginTable("##roster", voiceOn ? 5 : 4, tflags)) {
            // The column words are looked up once; the setup calls take them with the English
            // word as the id, and the hand-drawn header row below draws the bare text.
            const char* const wPlayer = l10n::T("Player");
            const char* const wMic    = l10n::T("Mic");
            const char* const wLink   = l10n::T("Link");
            const char* const wPing   = l10n::T("Ping");
            const char* const wId     = l10n::T("ID");
            ImGui::TableSetupColumn(l10n::Label(wPlayer, "player"), ImGuiTableColumnFlags_WidthStretch);
            // The voice-state icon: a click on the self row toggles mute, on a remote row opens the
            // per-player volume popup.
            if (voiceOn)
                ImGui::TableSetupColumn(l10n::Label(wMic, "mic"), ImGuiTableColumnFlags_WidthFixed, S(40.0f));
            // The connection: how this player reaches the session (LAN, direct, relay), or n/a on
            // the host row, whose traffic never crosses a socket. Host-measured and host-published,
            // so every board shows the same value for the same player.
            ImGui::TableSetupColumn(l10n::Label(wLink, "link"), ImGuiTableColumnFlags_WidthFixed, S(84.0f));
            ImGui::TableSetupColumn(l10n::Label(wPing, "ping"), ImGuiTableColumnFlags_WidthFixed, S(66.0f));
            // The ID: the occupant's session number, host-issued and never reused within a session.
            // Not the slot, which names a seat rather than a person, and not shown on the
            // nameplate; the player list is where the full information lives. Far right, so the
            // name leads the row.
            ImGui::TableSetupColumn(l10n::Label(wId, "id"), ImGuiTableColumnFlags_WidthFixed, S(44.0f));
            // The headers are drawn by hand rather than with the table's helper, which left-aligns
            // every label, so the numeric labels sat on the left of their right-aligned values;
            // each header takes the alignment of the data below it. The headers row flag still
            // paints the background.
            {
                const ImVec4 hdrCol(0.62f, 0.71f, 0.82f, 1.0f);
                int hc = 0;
                auto headerCell = [&](const char* label, bool rightAlign) {
                    ImGui::TableSetColumnIndex(hc++);
                    if (rightAlign) {
                        const float tw = ImGui::CalcTextSize(label).x;
                        const float cw = ImGui::GetContentRegionAvail().x;
                        if (cw > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw));
                    }
                    ImGui::TextColored(hdrCol, "%s", label);
                };
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                headerCell(wPlayer, false);
                if (voiceOn) headerCell(wMic, false);
                headerCell(wLink, false);
                headerCell(wPing, true);
                headerCell(wId, true);
            }
            for (int i = 0; i < s.count; ++i) {
                const coop::roster::Row& r = s.rows[i];
                const char* nick = r.nick[0] ? r.nick : (r.isLocal ? wPlayer : l10n::T("Remote player"));
                // A custom nick colour (a synced preference) overrides the role colour; the role
                // stays readable through the HOST tag. The local row resolves through the local
                // preference, since our own slot never receives its colour over the wire.
                const uint32_t custom = r.isLocal ? coop::nick_color::LocalPacked()
                                                  : coop::nick_color::PackedForSlot(r.slot);
                const ImVec4 nickCol =
                    coop::nick_color::IsCustom(custom)
                        ? ImVec4(coop::nick_color::R(custom) / 255.f,
                                 coop::nick_color::G(custom) / 255.f,
                                 coop::nick_color::B(custom) / 255.f, 1.0f)
                        : r.isHost ? ImVec4(1.00f, 0.82f, 0.35f, 1.0f)   // host  = gold
                                   : ImVec4(0.86f, 0.89f, 0.94f, 1.0f);  // client = soft white
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(r.slot);
                StatusDot(r.connected);
                ImGui::SameLine();

                const bool actionable = host && !r.isLocal && r.connected && r.slot >= 1;
                if (actionable) {
                    ImGui::PushStyleColor(ImGuiCol_Text, nickCol);
                    // DontClosePopups, since clicking the row toggles the popup and the selection
                    // state is meaningless; AllowOverlap, since the row spans all columns and a
                    // click goes to the first submitted item, so without it the row would eat the
                    // mic icon's clicks.
                    if (ImGui::Selectable(nick, false,
                                          ImGuiSelectableFlags_DontClosePopups |
                                              ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowOverlap)) {
                        std::snprintf(g_actId, sizeof(g_actId), "%s", r.playerId);
                        std::snprintf(g_actNick, sizeof(g_actNick), "%s", nick);
                        ImGui::OpenPopup("##act");
                    }
                    IdTooltip(r, host);
                    ImGui::PopStyleColor();

                    if (ImGui::BeginPopup("##act")) {
                        ImGui::TextDisabled("%s", g_actNick);
                        ImGui::Separator();
                        // Host-standard actions, not dev-gated: each submits a command line for
                        // the person the popup opened on. The line names the proved id, which
                        // survives the seat changing hands; the command's permission check and
                        // its reply (in the host's chat) are the same as when it is typed.
                        const bool hasId = g_actId[0] != '\0';
                        if (IdMenuItem(l10n::T("Teleport to me"), "teleport_to_me", hasId))
                            coop::command_sync::Submit(std::string("tphere ") + g_actId);
                        if (IdMenuItem(l10n::Tc("hud", "Kick"), "kick", hasId))
                            coop::command_sync::Submit(std::string("kick ") + g_actId);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
                        const bool banClicked = IdMenuItem(l10n::T("Ban (permanent)"), "ban_permanent", hasId);
                        ImGui::PopStyleColor();
                        if (banClicked) {
                            // The confirm modal executes after an arbitrary typing delay, exactly
                            // the window a slot can change hands in, so the id is stashed and the
                            // ban aimed at the person.
                            std::snprintf(g_banConfirmId, sizeof(g_banConfirmId), "%s", g_actId);
                            std::snprintf(g_banConfirmNick, sizeof(g_banConfirmNick), "%s", g_actNick);
                            g_banConfirmByAddress = true;
                        }
                        ImGui::EndPopup();
                    }
                } else {
                    ImGui::TextColored(nickCol, "%s", nick);
                    IdTooltip(r, host);
                    if (r.isLocal) { ImGui::SameLine(0.0f, S(6.0f)); ImGui::TextDisabled("%s", l10n::T("(you)")); }
                    // HOST belongs on the name, not in the link column: it is a fact about who the
                    // player is, while the link column answers how their traffic reaches the
                    // session, and it explains why the host's link and ping cells read n/a. A host
                    // row is never actionable, so this is its only path.
                    if (r.isHost) {
                        ImGui::SameLine(0.0f, S(6.0f));
                        ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.35f, 0.85f), "%s", l10n::T("HOST"));
                    }
                }

                // The voice column, the per-player mic-state icon: the self row toggles your mute,
                // a remote row opens the local mute and volume popup.
                int col = 1;  // 0 = Player; Mic/Link/Ping/ID follow in that order
                if (voiceOn) {
                    ImGui::TableSetColumnIndex(col++);
                    const bool self = r.isLocal;
                    const auto icon = static_cast<coop::voice_chat::VoiceIcon>(
                        r.slot >= 0 && r.slot < static_cast<int>(coop::players::kMaxPeers)
                            ? vs.icons[r.slot] : 0);
                    const float ih = ImGui::GetTextLineHeight();
                    const ImVec2 cell = ImGui::GetCursorScreenPos();
                    // A locally silenced peer shows the muted glyph dimmed even when its own state
                    // is none.
                    const bool localSilenced =
                        !self && r.slot >= 0 &&
                        r.slot < static_cast<int>(coop::players::kMaxPeers) &&
                        vs.slotVolume[r.slot] <= 0.001f;
                    if (ImGui::InvisibleButton("##vicon", ImVec2(S(20.0f), ih))) {
                        if (self) coop::voice_chat::SetMuted(vs.muted == 0);
                        else if (r.connected) ImGui::OpenPopup("##vvol");
                    }
                    // An idle peer draws a dim mic outline instead of nothing: an invisible cell
                    // read as no mute icons and gave the click target no affordance.
                    const bool idle =
                        !localSilenced && icon == coop::voice_chat::VoiceIcon::None;
                    const auto shown = localSilenced ? coop::voice_chat::VoiceIcon::MicMuted
                                       : idle        ? coop::voice_chat::VoiceIcon::Talking
                                                     : icon;
                    ui::voice_icons::Draw(ImGui::GetWindowDrawList(),
                                          ImVec2(cell.x + S(10.0f), cell.y + ih * 0.5f),
                                          ih * 0.95f, shown,
                                          localSilenced ? 0.55f : idle ? 0.30f : 1.0f);
                    if (ImGui::IsItemHovered()) {
                        const char* lbl = localSilenced ? l10n::T("Muted for you (click for volume)")
                                          : self ? (vs.muted ? l10n::T("Your mic is muted -- click to unmute")
                                                             : l10n::T("Click to mute your mic"))
                                          : idle ? l10n::T("Voice connected (click for volume)")
                                                 : ui::voice_icons::Label(shown);
                        if (lbl && lbl[0]) ImGui::SetTooltip("%s", lbl);
                    }
                    if (!self && ImGui::BeginPopup("##vvol")) {
                        ImGui::TextDisabled("%s", nick);
                        ImGui::Separator();
                        float v = vs.slotVolume[r.slot];
                        bool silenced = v <= 0.001f;
                        if (ImGui::Checkbox(l10n::Label(l10n::T("Mute for me"), "mute_for_me"), &silenced))
                            coop::voice_chat::SetSlotVolume(r.slot, silenced ? 0.0f : 1.0f);
                        if (!silenced) {
                            if (ImGui::SliderFloat("##pvol", &v, 0.0f, 2.0f, "volume %.2fx"))
                                coop::voice_chat::SetSlotVolume(r.slot, v);
                        }
                        ImGui::EndPopup();
                    }
                }

                // The link column, through the one shared renderer the scoreboard, the admin panel
                // and the nameplate use.
                ImGui::TableSetColumnIndex(col++);
                ImGui::TextDisabled("%s", ui::link_format::LinkLabel(r.linkKind));

                // The ping column, right-aligned. Every row renders, your own included: the host
                // measures your link and publishes it, so your own ping is a real number and
                // belongs on your row.
                ImGui::TableSetColumnIndex(col++);
                {
                    char pb[16];
                    ui::link_format::FormatPing(r.ping, r.linkKind, pb, sizeof(pb));
                    const float tw = ImGui::CalcTextSize(pb).x;
                    const float cw = ImGui::GetContentRegionAvail().x;
                    if (cw > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw));
                    ImGui::TextDisabled("%s", pb);
                }

                // The ID column, last and right-aligned, so the numbers form a clean edge against
                // the ping cell.
                ImGui::TableSetColumnIndex(col);
                {
                    char ib[16];
                    if (r.playerNo != coop::roster::kNoPlayerNo)
                        std::snprintf(ib, sizeof(ib), "%u", static_cast<unsigned>(r.playerNo));
                    else
                        std::snprintf(ib, sizeof(ib), "-");  // out of session: none issued
                    const float tw = ImGui::CalcTextSize(ib).x;
                    const float cw = ImGui::GetContentRegionAvail().x;
                    if (cw > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw));
                    ImGui::TextDisabled("%s", ib);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();     // CellPadding
        ImGui::PopStyleColor(2);  // TableRowBgAlt + TableBorderLight
        if (s.count == 0) ImGui::TextDisabled("%s", l10n::T("No players."));

        // The host-only session block. A session is configured when it is created and not
        // afterwards: the password, the lock and the connection are settled in the hosting flow,
        // and changing them means hosting again. Hiding is the one exception, for a measured
        // reason: for a relay lobby the master is the only rendezvous, so hiding at creation would
        // make the game unjoinable, and the choice can only exist after friends are in; a direct
        // lobby makes the same choice at host time. So one control, and everything else about the
        // session is shown, never edited.
        if (host) {
            ImGui::Separator();
            bool listed = coop::session_manager::ListedState();
            if (ImGui::Checkbox(l10n::Label(l10n::T("Show in server browser"), "show_in_server_browser"),
                                &listed))
                coop::session_manager::SetListed(listed);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", listed ? l10n::T("(others can find your game)")
                                             : l10n::T("(hidden -- friends join by invite/IP)"));
        }

        // The ban confirmation modal, shared across rows: a ban is destructive and irreversible
        // from the UI, so it gets an explicit confirm step. Opened once on the transition, closed
        // by either button.
        if (g_banConfirmId[0] != '\0' && !ImGui::IsPopupOpen(kBanModalId))
            ImGui::OpenPopup(kBanModalId);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(l10n::Label(l10n::T("Confirm ban"), "coop"), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            char prompt[256];
            l10n::Fmt(prompt, sizeof(prompt), l10n::T("Permanently ban %s?"), g_banConfirmNick);
            ImGui::TextUnformatted(prompt);
            ImGui::TextDisabled("%s", l10n::T("Disconnected now and refused whenever they rejoin."));
            ImGui::Checkbox(l10n::Label(l10n::T("Also refuse their address"), "also_refuse_their_address"),
                            &g_banConfirmByAddress);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", l10n::T("Also refuses anyone connecting from the same address -- people who share "
                                                "their router or their provider's address are refused too. It only works "
                                                "on a direct connection: a player who comes through a relay is refused by "
                                                "their identity alone."));
            ImGui::Spacing();
            if (ImGui::Button(l10n::Label(l10n::Tc("hud", "Ban"), "ban"), ImVec2(S(110.f), 0))) {
                coop::command_sync::Submit(std::string(g_banConfirmByAddress ? "ban " : "banid ") +
                                           g_banConfirmId);
                g_banConfirmId[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(l10n::Label(l10n::Tc("hud", "Cancel"), "cancel"), ImVec2(S(110.f), 0))) {
                g_banConfirmId[0] = '\0';
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
