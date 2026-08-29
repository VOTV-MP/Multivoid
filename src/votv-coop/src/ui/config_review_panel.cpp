// ui/config_review_panel.cpp -- see ui/config_review_panel.h.

#include "ui/config_review_panel.h"

#include "coop/config/config_review.h"
#include "ui/menu_sfx.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ui::config_review_panel {

using ui::scale::S;

namespace {

namespace CR = coop::config_review;

// Last Tidy-press outcome, rendered as a status line so the button is never
// silent (user 2026-07-26: pressing it looked dead -- the old reformat moved
// layout only, so the panel's rows survived every press with zero feedback).
bool               g_tidyPressed = false;
CR::ReformatOutcome g_tidyOutcome;

const char* TypeHeading(CR::Row::Type t) {
    switch (t) {
        case CR::Row::Type::Rejected:          return "Ignored values (using the default instead)";
        case CR::Row::Type::Unknown:           return "Unknown settings (nothing reads these)";
        case CR::Row::Type::DuplicateDormant:  return "Duplicate settings with different values";
        case CR::Row::Type::IdentityNotDurable: return "Player identity not saved";
        case CR::Row::Type::IniUnreadable:     return "multivoid.ini could not be read";
    }
    return "?";
}

void RenderRowsOfType(const std::vector<CR::Row>& rows, CR::Row::Type type) {
    bool headed = false;
    for (const auto& r : rows) {
        if (r.type != type) continue;
        if (!headed) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
            ImGui::TextUnformatted(TypeHeading(type));
            ImGui::PopStyleColor();
            headed = true;
        }
        switch (type) {
            case CR::Row::Type::Rejected:
                ImGui::Bullet();
                ImGui::TextWrapped("%s = '%s' (%s) -- %s; the built-in default is used.",
                                   r.key.c_str(), r.value.c_str(), r.origin.c_str(),
                                   r.reason.c_str());
                break;
            case CR::Row::Type::Unknown:
                ImGui::Bullet();
                if (!r.reason.empty()) {
                    // A key this build deliberately retired -- say where it went,
                    // and do not show the VALUE: the one that put this branch here
                    // was a player identity, and a settings popup is exactly the
                    // screenshot people paste into a bug report.
                    ImGui::TextWrapped("%s -- %s", r.key.c_str(), r.reason.c_str());
                } else {
                    ImGui::TextWrapped("%s = '%s' -- not a known setting (typo, or a retired key).",
                                       r.key.c_str(), r.value.c_str());
                }
                break;
            case CR::Row::Type::DuplicateDormant: {
                ImGui::Bullet();
                ImGui::TextWrapped("%s appears %d times with different values; the FIRST line "
                                   "(line %d) wins. Pick which one to keep:",
                                   r.key.c_str(), static_cast<int>(r.dupLines.size()),
                                   r.dupLines.empty() ? 0 : r.dupLines[0].lineNo);
                if (r.identityKey)
                    ImGui::TextWrapped("   This key IS your player identity -- if the winning "
                                       "value is not the one you expect, keep the other line.");
                for (const auto& dl : r.dupLines) {
                    char btn[192];
                    std::snprintf(btn, sizeof(btn), "Keep line %d: %s###cfgrev_%s_%d",
                                  dl.lineNo, dl.value.c_str(), r.key.c_str(), dl.lineNo);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(24.f));
                    // Correlated by VALUE, not the shown line number (CRIT-2:
                    // the snapshot ages while the panel sits on screen).
                    if (ui::menu_sfx::Button(btn, ImVec2(0, 0)))
                        CR::KeepDuplicateLine(r.key, dl.value);
                }
                break;
            }
            case CR::Row::Type::IdentityNotDurable:
                ImGui::Bullet();
                ImGui::TextWrapped("Your player identity (player_guid / player_skin) could not "
                                   "be saved to multivoid.ini this launch -- it is temporary "
                                   "for this session. Fix whatever locks the file (editor, "
                                   "antivirus, permissions); nothing is lost once a launch can "
                                   "save again.");
                break;
            case CR::Row::Type::IniUnreadable:
                ImGui::Bullet();
                ImGui::TextWrapped("multivoid.ini exists but could not be read (locked or "
                                   "failing). This launch runs on environment overrides and "
                                   "built-in defaults.");
                break;
        }
    }
}

}  // namespace

bool IsOpen() { return CR::PanelOpen(); }

void Render() {
    std::vector<CR::Row> rows;
    CR::GetSnapshot(rows);
    if (rows.empty()) return;

    ui::menu_sfx::FrameBegin();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(520.f), 0),
                                        ImVec2(S(760.f), io.DisplaySize.y * 0.8f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18.0f), S(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.09f, 0.11f, 0.97f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("###coop_config_review", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("MULTIVOID SETTINGS CHECK");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "multivoid.ini was checked against the known settings at launch.\n"
                "Nothing was changed automatically -- broken values just fall back\n"
                "to their defaults for this session. The buttons below edit the file\n"
                "only when you click them. This notice returns next launch while\n"
                "anything is still off.");
        ImGui::Separator();

        RenderRowsOfType(rows, CR::Row::Type::IniUnreadable);
        RenderRowsOfType(rows, CR::Row::Type::IdentityNotDurable);
        RenderRowsOfType(rows, CR::Row::Type::DuplicateDormant);
        RenderRowsOfType(rows, CR::Row::Type::Rejected);
        RenderRowsOfType(rows, CR::Row::Type::Unknown);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ui::menu_sfx::Button("Tidy up multivoid.ini###cfgrev_reformat",
                                 ImVec2(S(220.f), S(30.f)))) {
            g_tidyOutcome = CR::ReformatNow();
            g_tidyPressed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Rewrites multivoid.ini in the standard layout: [net] first, [dev]\n"
                "last, each setting under its section, exact-duplicate lines merged.\n"
                "Unknown settings and invalid values are commented out (kept in the\n"
                "file, just disabled) -- that resolves their warnings above.\n"
                "Conflicting duplicates are NEVER auto-resolved -- use the keep-line\n"
                "buttons above. Your comments travel with their settings.");
        ImGui::SameLine();
        if (ui::menu_sfx::Button("Dismiss###cfgrev_dismiss", ImVec2(S(120.f), S(30.f))))
            CR::Dismiss();
        if (g_tidyPressed) {
            ImGui::Spacing();
            if (!g_tidyOutcome.ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::TextWrapped("Could not rewrite multivoid.ini (locked or unreadable) -- "
                                   "nothing was changed. Close whatever holds the file and "
                                   "press Tidy up again.");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.55f, 1.0f));
                ImGui::TextWrapped("Tidied: %d duplicate line(s) merged, %d setting(s) placed "
                                   "under their sections, %d unknown/invalid line(s) commented "
                                   "out. Anything still listed above needs a choice or can't "
                                   "be fixed from the file.",
                                   g_tidyOutcome.collapsed, g_tidyOutcome.placed,
                                   g_tidyOutcome.retired);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::menu_sfx::FrameEnd();
}

}  // namespace ui::config_review_panel
