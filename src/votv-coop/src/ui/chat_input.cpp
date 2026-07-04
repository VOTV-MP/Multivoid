// ui/chat_input.cpp -- see ui/chat_input.h.

#include "ui/chat_input.h"

#include "coop/comms/chat_sync.h"
#include "ui/fonts.h"

#include "imgui.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <string>

namespace ui::chat_input {
namespace {

std::atomic<bool> g_open{false};
char g_buf[204] = {};          // render-thread only (matches ChatMessagePayload.text + NUL)
bool g_focusPending = false;   // focus the field on the first frame after Open

// Up/Down send-history recall (2026-07-04, the chat-imgui-samp shape): the last
// kHistoryMax sent lines, newest LAST. g_histPos = -1 means "live" (not browsing);
// entering history stashes the live text so Down past the newest restores it.
// Render thread only (the InputText callback + Render run there).
constexpr size_t kHistoryMax = 16;
std::deque<std::string> g_history;
int g_histPos = -1;
std::string g_liveStash;

int HistoryCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty()) return 0;
    const int last = static_cast<int>(g_history.size()) - 1;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (g_histPos < 0) { g_liveStash = data->Buf; g_histPos = last; }
        else if (g_histPos > 0) --g_histPos;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (g_histPos < 0) return 0;      // already live
        if (g_histPos < last) ++g_histPos;
        else {                             // past the newest -> restore the stash
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, g_liveStash.c_str());
            g_histPos = -1;
            return 0;
        }
    } else {
        return 0;
    }
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, g_history[static_cast<size_t>(g_histPos)].c_str());
    return 0;
}

}  // namespace

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void Open() {
    g_buf[0] = '\0';
    g_focusPending = true;
    g_histPos = -1;
    g_open.store(true, std::memory_order_relaxed);
}

void Close() {
    g_open.store(false, std::memory_order_relaxed);
    g_buf[0] = '\0';
    g_histPos = -1;
}

void Render() {
    if (!IsOpen()) return;
    // ESC -> close WITHOUT sending. The WndProc also closes on ESC before the
    // capture-swallow (so the game pause menu opens normally); this in-frame
    // check is the belt-and-braces for paths where the keydown reached ImGui.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { Close(); return; }

    ImFont* chatFont = ui::fonts::Chat();
    if (chatFont) ImGui::PushFont(chatFont);

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
        ImGui::SetNextItemWidth(560.f);
        if (g_focusPending) { ImGui::SetKeyboardFocusHere(); g_focusPending = false; }
        const bool submitted = ImGui::InputText(
            "##chatline", g_buf, sizeof(g_buf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
            &HistoryCallback);
        if (submitted) {
            if (g_buf[0] != '\0') {
                coop::chat_sync::QueueSend(std::string(g_buf));
                if (g_history.empty() || g_history.back() != g_buf) {
                    g_history.emplace_back(g_buf);
                    while (g_history.size() > kHistoryMax) g_history.pop_front();
                }
            }
            Close();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Enter = send, Esc = close, Up/Down = history)");
    }
    ImGui::End();

    if (chatFont) ImGui::PopFont();
}

}  // namespace ui::chat_input
