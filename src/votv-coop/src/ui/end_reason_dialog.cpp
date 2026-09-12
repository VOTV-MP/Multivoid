// ui/end_reason_dialog.cpp -- see ui/end_reason_dialog.h.

#include "ui/end_reason_dialog.h"

#include "coop/net/end_reason.h"
#include "coop/session/join_progress.h"  // PeekNotice / ClearNotice (the notice owner)
#include "ui/menu_sfx.h"                  // native VOTV button click + rollover sounds
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace ui::end_reason_dialog {

using ui::scale::S;

bool IsOpen() {
    // Lock-free gate (called every overlay frame from imgui_overlay's render pass +
    // AnyOpen/CaptureActive); the mutex is taken only in Render's actual read.
    return coop::join_progress::NoticePending();
}

void Render() {
    coop::join_progress::Notice notice;
    if (!coop::join_progress::PeekNotice(notice)) return;  // nothing pending
    const coop::net::EndReasonInfo& info = coop::net::Describe(notice.code);
    ui::menu_sfx::FrameBegin();  // arm per-frame hover-enter detection for the sfx button

    const ImGuiIO& io = ImGui::GetIO();

    // Dim backdrop over the whole screen (including the reopened browser beneath) so the
    // box reads as foregrounded. Purely VISUAL (BACKGROUND draw list, no input capture) --
    // the browser behind stays clickable, which is fine: clicking Connect there starts a
    // fresh join, and BeginConnect clears this notice. OK just acknowledges + hides.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, 130));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Width comes from PushTextWrapPos(404) + WindowPadding under AlwaysAutoResize
    // (an explicit SetNextWindowSize would be ignored by the auto-resize flag).

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18.0f), S(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.06f, 0.07f, 0.98f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("###coop_end_reason", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        // A join that never completed could not connect; a link that ended after it disconnected.
        ImGui::TextUnformatted(notice.afterJoin ? "DISCONNECTED" : "COULD NOT CONNECT");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(S(404.0f));
        // The code's own sentence first; the site's text under it when it adds something (the
        // two builds, the host's own words), never the same line twice.
        ImGui::TextWrapped("%s", info.text);
        if (!notice.detail.empty() && notice.detail != info.text) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.66f, 0.66f, 1.0f));
            ImGui::TextWrapped("%s", notice.detail.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        // The stable code, so a report names the site without a log.
        char code[96];
        std::snprintf(code, sizeof(code), "Code %s -- include it in a bug report", info.id);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.56f, 0.58f, 1.0f));
        ImGui::TextWrapped("%s", code);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();

        const float bw = S(120.0f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
        if (ui::menu_sfx::Button("OK###coop_er_ok", ImVec2(bw, S(30.0f)))) {
            coop::join_progress::ClearNotice();  // acknowledge -> hide next frame
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::menu_sfx::FrameEnd();
}

}  // namespace ui::end_reason_dialog
