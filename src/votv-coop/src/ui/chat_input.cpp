// ui/chat_input.cpp -- see ui/chat_input.h.

#include "ui/chat_input.h"

#include "coop/chat_sync.h"

#include "imgui.h"

#include <atomic>
#include <cstring>
#include <string>

namespace ui::chat_input {
namespace {

std::atomic<bool> g_open{false};
char g_buf[204] = {};          // render-thread only (matches ChatMessagePayload.text + NUL)
bool g_focusPending = false;   // focus the field on the first frame after Open

}  // namespace

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void Open() {
    g_buf[0] = '\0';
    g_focusPending = true;
    g_open.store(true, std::memory_order_relaxed);
}

void Close() {
    g_open.store(false, std::memory_order_relaxed);
    g_buf[0] = '\0';
}

void Render() {
    if (!IsOpen()) return;
    // ESC -> close WITHOUT sending. The WndProc also closes on ESC before the
    // capture-swallow (so the game pause menu opens normally); this in-frame
    // check is the belt-and-braces for paths where the keydown reached ImGui.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { Close(); return; }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float pad = 14.f;
    // Sit just under the chat feed (the feed's bottom edge is at 0.5 of the
    // screen height -- ui/hud.cpp DrawChat kBottomFrac).
    ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y * 0.5f + 6.f),
                            ImGuiCond_Always, ImVec2(0.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##coop_chat_input", nullptr, flags)) {
        ImGui::TextDisabled("say:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(420.f);
        if (g_focusPending) { ImGui::SetKeyboardFocusHere(); g_focusPending = false; }
        const bool submitted = ImGui::InputText("##chatline", g_buf, sizeof(g_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted) {
            if (g_buf[0] != '\0') coop::chat_sync::QueueSend(std::string(g_buf));
            Close();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Enter = send, Esc = close)");
    }
    ImGui::End();
}

}  // namespace ui::chat_input
