// ui/boot_warning_dialog.cpp -- see ui/boot_warning_dialog.h.

#include "ui/boot_warning_dialog.h"

#include "ui/menu_sfx.h"  // native VOTV button click + rollover sounds
#include "ui/scale.h"

#include "imgui.h"

#include <atomic>
#include <mutex>
#include <string>

namespace ui::boot_warning_dialog {

using ui::scale::S;

namespace {

// Armed once by the boot thread, cleared by the OK button. The atomic mirrors
// "text non-empty" so the per-frame IsOpen() gate never takes the mutex (same
// shape as join_progress's g_failPending).
std::mutex g_mu;
std::string g_text;
std::atomic<bool> g_pending{false};

}  // namespace

void Arm(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_text = text;
    g_pending.store(true, std::memory_order_release);
}

bool IsOpen() { return g_pending.load(std::memory_order_relaxed); }

void Clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_text.clear();
    g_pending.store(false, std::memory_order_release);
}

void Render() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_text.empty()) return;
        text = g_text;
    }
    ui::menu_sfx::FrameBegin();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, 130));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18.0f), S(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.08f, 0.05f, 0.98f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("###coop_boot_warning", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::TextUnformatted("MOD INSTALL PROBLEM");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // ASCII by construction (filenames the proxy found + our fixed phrasing).
        ImGui::PushTextWrapPos(S(440.0f));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();

        const float bw = S(120.0f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
        if (ui::menu_sfx::Button("OK###coop_bw_ok", ImVec2(bw, S(30.0f)))) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_text.clear();
            g_pending.store(false, std::memory_order_release);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::menu_sfx::FrameEnd();
}

}  // namespace ui::boot_warning_dialog
