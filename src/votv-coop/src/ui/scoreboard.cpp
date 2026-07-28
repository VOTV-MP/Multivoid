// ui/scoreboard.cpp -- see ui/scoreboard.h.

#include "ui/scoreboard.h"

#include "coop/moderation/moderation.h"
#include "coop/session/session_manager.h"  // ListedState / SetListed (the host hide toggle)
#include "coop/player/nick_color.h"
#include "coop/player/roster.h"
#include "coop/voice/voice_chat.h"
#include "ui/link_format.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace ui::scoreboard {
namespace {

using ui::scale::S;

// Pending permanent-ban confirmation (render-thread only). >=0 == a ban is
// awaiting the modal's confirm; the nick is copied for the prompt text so the
// modal survives the row's roster snapshot changing under it.
//
// The TOKEN is stashed alongside, and it is the load-bearing part: the modal sits
// open across an arbitrary typing delay, and slots recycle. Aiming the ban at
// g_banConfirmSlot alone meant a permanent IP ban could land on whoever inherited
// the seat while the admin was typing.
int  g_banConfirmSlot = -1;
char g_banConfirmNick[24] = {};
coop::moderation::PlayerToken g_banConfirmToken{};

// A small filled status dot drawn inline before a name (green = connected). Uses
// the window draw list + a Dummy spacer so the following SameLine() name lands
// just to its right, vertically centred on the text line.
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

}  // namespace

bool LocalIsHost() { return coop::roster::LocalIsHost(); }  // lock-free (overlay hot path)

void Render() {
    coop::roster::Snapshot s;
    coop::roster::GetSnapshot(s);

    const ImGuiIO& io = ImGui::GetIO();
    // Top-centre, pinned. Pivot (0.5, 0) keeps it centred regardless of width.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.14f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));  // upper-centre, below the top HUD
    // 540 wide, auto height. 300 -> 460 in 2026-06-12 (truncated nicks + a clipped
    // "LAN HOST"); 460 -> 540 in 2026-07-28 once the columns got HEADERS -- at 460
    // the labels had less room than the values under them, so "Mic" bled into
    // "Link" and the whole strip read as one run of words (user: "текст над
    // колонками уходит на сразу несколько колонок").
    ImGui::SetNextWindowSize(ImVec2(S(540.0f), 0.0f), ImGuiCond_Always);

    // Clean translucent panel: padded, rounded, borderless, dark bg so it reads over
    // any scene. Own header (no OS-style title bar).
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
        // Header: bright "PLAYERS" + accent online count + a dim "V: voice" key
        // hint (the settings window moved to its own V-key surface, user
        // 2026-06-12 round 1 -- no button here).
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.00f, 1.0f));
        ImGui::TextUnformatted("PLAYERS");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, S(8.0f));
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
        // Selectable that opens an action popup (Teleport-to-me / Kick / Ban -- all
        // host-standard admin verbs, no dev gate). A client's board (and the host's
        // own row) is plain text -- the client peek is passive (no input capture).
        const bool host = LocalIsHost();
        // BordersInnerH + a lifted alt-row tint: with RowBg alone the stripes were
        // invisible over the dark overlay and the rows read as one block (user
        // 2026-07-27, on the first screenshot of this surface).
        const ImGuiTableFlags tflags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerH;
        const bool voiceOn = vs.enabled != 0 && vs.started != 0;
        // First attempt used 0.055 / 0.10 alpha and was still unreadable in a
        // screenshot -- over a dark, busy 3D backdrop a stripe has to be far
        // stronger than it would need to be on a flat UI. Three cues now, not one:
        // a visible alternating tint, a real separator line, and vertical breathing
        // room so the rows are not one block of text.
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1.0f, 1.0f, 1.0f, 0.28f));
        // Roomier cells (was 4x5): with a header row above them the table needed
        // vertical air, and the horizontal pad is what keeps each label off its
        // neighbour's column edge.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8.0f), S(7.0f)));
        if (ImGui::BeginTable("##roster", voiceOn ? 5 : 4, tflags)) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
            // Voice state icon (v66; the user's mute-icon-on-playerlist ask).
            // Click = self: toggle mute; remote: per-player volume popup.
            if (voiceOn) ImGui::TableSetupColumn("Mic", ImGuiTableColumnFlags_WidthFixed, S(40.0f));
            // Connection (v131): how THIS PLAYER reaches the session -- "LAN" /
            // "DIRECT" / "RELAY", or "n/a" on the host row, whose traffic never
            // crosses a socket. Host-measured and host-published, so every board
            // shows the same value for the same player. It used to be derived
            // per viewer, which put transport on some rows and routing ("VIA
            // HOST") on others, in one column, side by side.
            ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthFixed, S(84.0f));
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, S(66.0f));
            // ID (arc A): the occupant's session number, host-issued and never
            // reused within a session. It is NOT the slot -- slots recycle, so a
            // slot number names a seat, not a person -- and it is deliberately
            // NOT shown on the nameplate (user 2026-07-27); TAB is where the full
            // information lives, SA-MP-shaped.
            //
            // FAR RIGHT (user 2026-07-27). It led the row at first, which put the
            // least-scanned field where the eye lands and pushed the NAME -- the
            // thing anyone actually looks for -- off the left edge.
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, S(44.0f));
            // The columns had NEVER been named (user 2026-07-27: "the rows are
            // not even named properly"). TableSetupColumn only DECLARES a name;
            // something has to draw it.
            //
            // Hand-drawn rather than ImGui::TableHeadersRow(), because that helper
            // left-aligns EVERY label: "Ping" and "ID" then sat on the left of the
            // right-aligned numbers underneath, so no label was clearly over its own
            // value (user 2026-07-28: "нечетко над своей областью/колонкой"). Each
            // header here takes the SAME alignment as the data below it, so a
            // label and its column read as one unit. TableNextRow's Headers flag
            // still paints the header background; we only replace the text.
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
                headerCell("Player", false);
                if (voiceOn) headerCell("Mic", false);
                headerCell("Link", false);
                headerCell("Ping", true);
                headerCell("ID", true);
            }
            for (int i = 0; i < s.count; ++i) {
                const coop::roster::Row& r = s.rows[i];
                const char* nick = r.nick[0] ? r.nick : (r.isLocal ? "Player" : "Remote player");
                // v103 (12f): a custom nick color (synced pref) overrides the role
                // color -- role stays readable via the HOST tag beside the name.
                // The local row resolves through the local pref directly (our own
                // slot never receives its color over the wire).
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
                        // Host-standard action, NOT dev-gated (user 2026-07-03: the host
                        // should always have it, like Kick/Ban -- it is an admin verb).
                        // Capture the TOKEN, not the slot. Slots recycle, so a
                        // slot number stops naming this person the moment they
                        // leave -- and these actions execute later, on the game
                        // thread, by which time the seat may have a new occupant.
                        const auto token =
                            coop::moderation::TokenFor(r.slot, r.playerNo, r.generation);
                        if (ImGui::MenuItem("Teleport to me"))
                            coop::moderation::TeleportPlayerToMe(token);
                        if (ImGui::MenuItem("Kick"))
                            coop::moderation::KickPlayer(token);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
                        const bool banClicked = ImGui::MenuItem("Ban (permanent)");
                        ImGui::PopStyleColor();
                        if (banClicked) {
                            // The confirm modal executes after an ARBITRARY typing
                            // delay, which is exactly the window a slot can change
                            // hands in. Stash the token so the ban is aimed at the
                            // person, not at the seat.
                            g_banConfirmToken = token;
                            g_banConfirmSlot = r.slot;
                            std::snprintf(g_banConfirmNick, sizeof(g_banConfirmNick), "%s", nick);
                        }
                        ImGui::EndPopup();
                    }
                } else {
                    ImGui::TextColored(nickCol, "%s", nick);
                    if (r.isLocal) { ImGui::SameLine(0.0f, S(6.0f)); ImGui::TextDisabled("(you)"); }
                    // HOST belongs on the NAME, not in the Link column. It is a
                    // fact about WHO this player is; the Link column answers how
                    // their traffic reaches the session, and the old "LAN HOST"
                    // fused the two. Without this marker the word would vanish
                    // from the UI entirely -- and it is what explains why the
                    // host's Link and Ping cells read "n/a". (A host row is never
                    // `actionable`, so this branch is its only path.)
                    if (r.isHost) {
                        ImGui::SameLine(0.0f, S(6.0f));
                        ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.35f, 0.85f), "HOST");
                    }
                }

                // Voice column (v66): the per-player mic-state icon. Self row:
                // click toggles your mute. Remote row: click opens the local
                // mute/volume popup (SetSlotVolume is render-thread-safe).
                int col = 1;  // 0 = Player; Mic/Link/Ping/ID follow in that order
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
                    if (ImGui::InvisibleButton("##vicon", ImVec2(S(20.0f), ih))) {
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
                                          ImVec2(cell.x + S(10.0f), cell.y + ih * 0.5f),
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

                // Link column: how THIS PLAYER reaches the session. One shared
                // renderer (ui::link_format) across scoreboard / admin panel /
                // nameplate -- three hand-copied cascades used to drift here.
                ImGui::TableSetColumnIndex(col++);
                ImGui::TextDisabled("%s", ui::link_format::LinkLabel(r.linkKind));

                // Ping column, right-aligned. EVERY row renders, including your
                // own: the host measures your link and publishes it, so your own
                // ping is a real number and belongs on your row. The old
                // `!r.isLocal` gate is exactly why the user saw their own row
                // blank ("the client himself doesn't see anything on himself").
                ImGui::TableSetColumnIndex(col++);
                {
                    char pb[16];
                    ui::link_format::FormatPing(r.ping, r.linkKind, pb, sizeof(pb));
                    const float tw = ImGui::CalcTextSize(pb).x;
                    const float cw = ImGui::GetContentRegionAvail().x;
                    if (cw > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw));
                    ImGui::TextDisabled("%s", pb);
                }

                // ID column, LAST and right-aligned so the numbers form a clean edge
                // instead of a ragged one against the ping cell.
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
            if (ImGui::Button("Ban", ImVec2(S(110.f), 0))) {
                coop::moderation::BanPlayer(g_banConfirmToken, "banned by host");
                g_banConfirmSlot = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(S(110.f), 0))) {
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
