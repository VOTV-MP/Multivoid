// ui/connect_failed_dialog.cpp -- see ui/connect_failed_dialog.h.

#include "ui/connect_failed_dialog.h"

#include "coop/session/join_progress.h"  // PeekFailReason / ClearFailReason (the reason owner)
#include "ui/menu_sfx.h"                  // native VOTV button click + rollover sounds
#include "ui/scale.h"

#include "imgui.h"

#include <string>

namespace ui::connect_failed_dialog {

using ui::scale::S;

bool IsOpen() {
    // Lock-free gate (called every overlay frame from imgui_overlay's render pass +
    // AnyOpen/CaptureActive); the mutex is taken only in Render's actual string read.
    return coop::join_progress::FailPending();
}

void Render() {
    std::string reason;
    if (!coop::join_progress::PeekFailReason(reason)) return;  // nothing pending
    ui::menu_sfx::FrameBegin();  // arm per-frame hover-enter detection for the sfx button

    const ImGuiIO& io = ImGui::GetIO();

    // Dim backdrop over the whole screen (including the reopened browser beneath) so the
    // box reads as foregrounded. Purely VISUAL (BACKGROUND draw list, no input capture) --
    // the browser behind stays clickable, which is fine: clicking Connect there starts a
    // fresh join, and BeginConnect clears this reason. OK just acknowledges + hides.
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
    if (ImGui::Begin("###coop_connect_failed", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextUnformatted("COULD NOT CONNECT");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // The reason is ASCII (see coop::join_progress::Fail call sites -- "Timed out
        // attempting to connect", "could not reach the server", ...). Wrap to the body.
        ImGui::PushTextWrapPos(S(404.0f));
        ImGui::TextWrapped("%s", reason.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();

        const float bw = S(120.0f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
        if (ui::menu_sfx::Button("OK###coop_cf_ok", ImVec2(bw, S(30.0f)))) {
            coop::join_progress::ClearFailReason();  // acknowledge -> hide next frame
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::menu_sfx::FrameEnd();
}

}  // namespace ui::connect_failed_dialog
