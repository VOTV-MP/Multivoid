// ui/config_review_panel.cpp -- see ui/config_review_panel.h.

#include "ui/config_review_panel.h"

#include "coop/config/config.h"           // why multivoid.ini could not be read
#include "coop/config/config_registry.h"  // which settings refuse instead of falling back
#include "coop/config/config_review.h"
#include "l10n/l10n.h"
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
// silent: a reformat that only moves layout leaves every panel row where it
// was, so without a status line the press looks dead.
bool               g_tidyPressed = false;
CR::ReformatOutcome g_tidyOutcome;

// The settings that refuse instead of falling back (config_registry::Row::failClosed), named in
// every sentence here that says what an unusable value leads to. From the registry, once.
const std::string& FailClosedNames() {
    static const std::string s = [] {
        std::string names;
        size_t count = 0;
        const coop::config_registry::Row* rows = coop::config_registry::Rows(count);
        for (size_t i = 0; i < count; ++i) {
            if (!rows[i].failClosed) continue;
            if (!names.empty()) names += ", ";
            names += rows[i].key;
        }
        return names;
    }();
    return s;
}

const char* TypeHeading(CR::Row::Type t) {
    switch (t) {
        case CR::Row::Type::Rejected:          return l10n::T("Values that could not be used");
        case CR::Row::Type::Unknown:           return l10n::T("Unknown settings (nothing reads these)");
        case CR::Row::Type::DuplicateDormant:  return l10n::T("Duplicate settings with different values");
        case CR::Row::Type::IdentityNotDurable: return l10n::T("Body skin not saved");
        case CR::Row::Type::IniUnreadable:     return l10n::T("multivoid.ini could not be read");
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
        char line[1024];  // one formatted sentence of the row; the row's own words are long
        switch (type) {
            case CR::Row::Type::Rejected:
                ImGui::Bullet();
                // What the value leads to: a set env twin shadows an ini line whatever it holds; a
                // fail-closed row has no default standing in, so what it governs is refused.
                if (!r.overriddenBy.empty())
                    l10n::Fmt(line, sizeof(line),
                              l10n::T("%1$s = '%2$s' (%3$s) -- %4$s; not in use, %5$s overrides it."),
                              r.key.c_str(), r.value.c_str(), r.origin.c_str(), r.reason.c_str(),
                              r.overriddenBy.c_str());
                else if (r.failClosed)
                    l10n::Fmt(line, sizeof(line),
                              l10n::T("%1$s = '%2$s' (%3$s) -- %4$s; nothing it governs starts until it is fixed."),
                              r.key.c_str(), r.value.c_str(), r.origin.c_str(), r.reason.c_str());
                else
                    l10n::Fmt(line, sizeof(line),
                              l10n::T("%1$s = '%2$s' (%3$s) -- %4$s; the built-in default is used."),
                              r.key.c_str(), r.value.c_str(), r.origin.c_str(), r.reason.c_str());
                ImGui::TextWrapped("%s", line);
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
                    l10n::Fmt(line, sizeof(line),
                              l10n::T("%1$s = '%2$s' -- not a known setting (typo, or a retired key)."),
                              r.key.c_str(), r.value.c_str());
                    ImGui::TextWrapped("%s", line);
                }
                break;
            case CR::Row::Type::DuplicateDormant: {
                ImGui::Bullet();
                const auto times = static_cast<unsigned long long>(r.dupLines.size());
                l10n::Fmt(line, sizeof(line),
                          l10n::Tn("%1$s appears %2$llu time with different values; the FIRST line "
                                   "(line %3$d) wins. Pick which one to keep:",
                                   "%1$s appears %2$llu times with different values; the FIRST line "
                                   "(line %3$d) wins. Pick which one to keep:",
                                   times),
                          r.key.c_str(), times, r.dupLines.empty() ? 0 : r.dupLines[0].lineNo);
                ImGui::TextWrapped("%s", line);
                if (r.identityKey)
                    ImGui::TextWrapped("   %s", l10n::T("This key holds your body skin -- if the winning "
                                                         "value is not the one you expect, keep the other line."));
                for (const auto& dl : r.dupLines) {
                    // The words are looked up; the id after "###" stays the runtime pair that names
                    // this key's line, so a translation never moves it.
                    char keep[256];
                    l10n::Fmt(keep, sizeof(keep), l10n::T("Keep line %1$d: %2$s"), dl.lineNo, dl.value.c_str());
                    char btn[320];
                    std::snprintf(btn, sizeof(btn), "%s###cfgrev_%s_%d",
                                  keep, r.key.c_str(), dl.lineNo);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(24.f));
                    // Correlated by VALUE, not the shown line number:
                    // the snapshot ages while the panel sits on screen.
                    if (ui::menu_sfx::Button(btn, ImVec2(0, 0)))
                        CR::KeepDuplicateLine(r.key, dl.value);
                }
                break;
            }
            case CR::Row::Type::IdentityNotDurable:
                ImGui::Bullet();
                l10n::Fmt(line, sizeof(line),
                          l10n::T("Your body skin (%s) could not be saved to "
                                  "multivoid.ini this launch -- it is temporary for this "
                                  "session. Fix whatever keeps the file from being read or "
                                  "saved (a lock from an editor or antivirus, permissions, or "
                                  "a file saved as UTF-16); nothing is lost once a "
                                  "launch can save again."),
                          "player_skin");
                ImGui::TextWrapped("%s", line);
                break;
            case CR::Row::Type::IniUnreadable:
                ImGui::Bullet();
                // From the row, not the live fault: the row says what the sweep saw, and a later
                // read, clean or locked, must not rewrite why it was raised.
                if (!r.unreadableNow) {
                    ImGui::TextWrapped("%s",
                                       r.fault == coop::config::IniFault::NotText
                                           ? l10n::T("multivoid.ini could not be read earlier this launch "
                                                     "(a zero byte or a UTF-16 byte-order mark) and reads now. "
                                                     "Settings read while it could not used their defaults; "
                                                     "restart so every setting reads the file.")
                                           : l10n::T("multivoid.ini could not be read earlier this launch "
                                                     "(locked or failing) and reads now. "
                                                     "Settings read while it could not used their defaults; "
                                                     "restart so every setting reads the file."));
                    break;
                }
                if (r.fault == coop::config::IniFault::NotText)
                    ImGui::TextWrapped("%s", l10n::T("multivoid.ini holds a zero byte or starts with a UTF-16 "
                                                     "byte-order mark, as a file saved as UTF-16 (\"Unicode\") "
                                                     "does. Save it again as UTF-8."));
                else
                    ImGui::TextWrapped("%s", l10n::T("multivoid.ini exists but could not be read (locked or "
                                                     "failing)."));
                l10n::Fmt(line, sizeof(line),
                          l10n::T("This launch runs on environment overrides and built-in "
                                  "defaults; the settings that refuse instead of falling back "
                                  "(%s) refuse what they govern until the file can be read."),
                          FailClosedNames().c_str());
                ImGui::TextWrapped("%s", line);
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
        ImGui::TextUnformatted(l10n::T("MULTIVOID SETTINGS CHECK"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            char tip[1024];
            l10n::Fmt(tip, sizeof(tip),
                      l10n::T("multivoid.ini was checked against the known settings at launch.\n"
                              "Nothing was changed automatically -- broken values fall back to\n"
                              "their defaults for this session, except the settings that refuse\n"
                              "instead (%s). The buttons below edit the file only when you click\n"
                              "them. This notice returns next launch while anything is still off."),
                      FailClosedNames().c_str());
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::Separator();

        RenderRowsOfType(rows, CR::Row::Type::IniUnreadable);
        RenderRowsOfType(rows, CR::Row::Type::IdentityNotDurable);
        RenderRowsOfType(rows, CR::Row::Type::DuplicateDormant);
        RenderRowsOfType(rows, CR::Row::Type::Rejected);
        RenderRowsOfType(rows, CR::Row::Type::Unknown);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ui::menu_sfx::Button(l10n::Label(l10n::T("Tidy up multivoid.ini"), "cfgrev_reformat"),
                                 ImVec2(S(220.f), S(30.f)))) {
            g_tidyOutcome = CR::ReformatNow();
            g_tidyPressed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            char tip[1024];
            l10n::Fmt(tip, sizeof(tip),
                      l10n::T("Rewrites multivoid.ini in the standard layout: [net] first, [dev]\n"
                              "last, each setting under its section, exact-duplicate lines merged.\n"
                              "Unknown settings and invalid values are commented out (kept in the\n"
                              "file, just disabled) -- that resolves their warnings above. An\n"
                              "invalid value of a setting that refuses instead (%s) is kept as\n"
                              "it is: disabling it would switch that setting to its default.\n"
                              "Conflicting duplicates are NEVER auto-resolved -- use the keep-line\n"
                              "buttons above. Your comments travel with their settings."),
                      FailClosedNames().c_str());
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::SameLine();
        if (ui::menu_sfx::Button(l10n::Label(l10n::Tc("hud", "Dismiss"), "cfgrev_dismiss"),
                                 ImVec2(S(120.f), S(30.f))))
            CR::Dismiss();
        if (g_tidyPressed) {
            ImGui::Spacing();
            if (!g_tidyOutcome.ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                if (g_tidyOutcome.fault == coop::config::IniFault::NotText)
                    ImGui::TextWrapped("%s", l10n::T("Could not rewrite multivoid.ini: it holds a zero byte or a "
                                                     "UTF-16 byte-order mark -- nothing was changed. Save it "
                                                     "again as UTF-8 and press Tidy up again."));
                else if (g_tidyOutcome.fault == coop::config::IniFault::ReadFailed)
                    ImGui::TextWrapped("%s", l10n::T("Could not rewrite multivoid.ini (locked or failing) -- "
                                                     "nothing was changed. Close whatever holds the file and "
                                                     "press Tidy up again."));
                else
                    ImGui::TextWrapped("%s", l10n::T("Could not rewrite multivoid.ini (it is missing, or the "
                                                     "write failed) -- nothing was changed."));
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.55f, 1.0f));
                // Three counts in one sentence: Tn takes one, so the "(s)" forms stay.
                char tidied[512];
                l10n::Fmt(tidied, sizeof(tidied),
                          l10n::T("Tidied: %1$d duplicate line(s) merged, %2$d setting(s) placed "
                                  "under their sections, %3$d unknown/invalid line(s) commented "
                                  "out. Anything still listed above needs a choice or can't "
                                  "be fixed from the file."),
                          g_tidyOutcome.collapsed, g_tidyOutcome.placed, g_tidyOutcome.retired);
                ImGui::TextWrapped("%s", tidied);
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
