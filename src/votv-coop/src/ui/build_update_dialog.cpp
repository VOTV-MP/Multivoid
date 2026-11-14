// ui/build_update_dialog.cpp -- see ui/build_update_dialog.h.

#include "ui/build_update_dialog.h"

#include "coop/net/build_share.h"
#include "coop/session/join_progress.h"  // ClearNotice: the join this build ended is not a failure to report
#include "l10n/l10n.h"
#include "ui/menu_sfx.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace ui::build_update_dialog {

using ui::scale::S;
namespace BS = coop::net::build_share;

bool IsOpen() {
    const BS::State s = BS::Peek();
    return s == BS::State::Offered || s == BS::State::Downloading || s == BS::State::Installed || s == BS::State::Failed;
}

void Render() {
    const BS::View v = BS::Snapshot();
    if (v.state == BS::State::Idle || v.state == BS::State::Asking) return;
    ui::menu_sfx::FrameBegin();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18.0f), S(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.11f, 0.98f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("###coop_build_update", nullptr, flags)) {
        char line[512];
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.0f, 1.0f));
        ImGui::TextUnformatted(l10n::T("THE HOST'S BUILD"));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(S(460.0f));
        const float mb = static_cast<float>(v.size) / (1024.0f * 1024.0f);
        if (v.state == BS::State::Offered) {
            l10n::Fmt(line, sizeof(line),
                      l10n::T("This host runs a different Multivoid build (b%1$d, %2$s). Your game can download it from the "
                              "host (%3$.0f MB) and use it from the next start of the game. Your current build is kept."),
                      static_cast<int>(v.hostProto), v.shaHex.c_str(), static_cast<double>(mb));
            ImGui::TextWrapped("%s", line);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.42f, 1.0f));
            ImGui::TextWrapped("%s", l10n::T("This is NOT an official release. It is whatever this host built, and it will run "
                                             "on your PC with full access. Accept it only from someone you trust."));
            ImGui::PopStyleColor();
            ImGui::Spacing();
            l10n::Fmt(line, sizeof(line), l10n::T("Host identity: %s"), v.hostGuid.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
            ImGui::TextWrapped("%s", line);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            if (ui::menu_sfx::Button(l10n::Label(l10n::T("Install"), "coop_bu_yes"), ImVec2(S(110.0f), S(30.0f))))
                BS::Accept(false);
            ImGui::SameLine();
            if (ui::menu_sfx::Button(l10n::Label(l10n::T("Install, and always trust this host"), "coop_bu_trust"),
                                     ImVec2(S(260.0f), S(30.0f))))
                BS::Accept(true);
            ImGui::SameLine();
            if (ui::menu_sfx::Button(l10n::Label(l10n::T("No"), "coop_bu_no"), ImVec2(S(70.0f), S(30.0f)))) BS::Decline();
        } else if (v.state == BS::State::Downloading) {
            ImGui::TextWrapped("%s", l10n::T("Downloading the host's build..."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            const float done = v.packed ? static_cast<float>(v.received) / static_cast<float>(v.packed) : 0.0f;
            std::snprintf(line, sizeof(line), "%.1f / %.1f MB", static_cast<double>(v.received) / (1024.0 * 1024.0),
                          static_cast<double>(v.packed) / (1024.0 * 1024.0));
            ImGui::ProgressBar(done, ImVec2(S(440.0f), S(22.0f)), line);
        } else {
            const bool installed = v.state == BS::State::Installed;
            if (installed) {
                // The join ended because this machine has to restart, not because it failed.
                coop::join_progress::ClearNotice();
                ImGui::TextWrapped("%s", l10n::T("The host's build is installed. Quit the game and start it again, then join "
                                                 "this host. Your previous build is kept beside it as main.dll.old."));
            } else {
                ImGui::TextWrapped("%s", l10n::T("The host's build was not installed. Your own build is unchanged."));
                if (!v.error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.66f, 0.66f, 1.0f));
                    ImGui::TextWrapped("%s", v.error.c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            const float bw = S(120.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
            if (ui::menu_sfx::Button(l10n::Label(l10n::Tc("hud", "OK"), "coop_bu_ok"), ImVec2(bw, S(30.0f)))) BS::Dismiss();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::menu_sfx::FrameEnd();
}

}  // namespace ui::build_update_dialog
