// ui/toast.cpp -- see ui/toast.h.

#include "ui/toast.h"

#include "imgui.h"

#include <chrono>
#include <mutex>
#include <vector>

namespace ui::toast {
namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
    std::string text;
    Clock::time_point deadline;
    bool warn = false;
};

std::mutex g_mu;  // Push happens on HTTP workers; Render on the render thread
std::vector<Entry> g_entries;

}  // namespace

void Push(const std::string& text, float seconds, bool warn) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_entries.size() >= 6) g_entries.erase(g_entries.begin());  // bounded
    g_entries.push_back(Entry{
        text,
        Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0f)),
        warn});
}

void Render() {
    std::vector<Entry> live;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto now = Clock::now();
        for (auto it = g_entries.begin(); it != g_entries.end();) {
            if (now >= it->deadline) it = g_entries.erase(it);
            else ++it;
        }
        if (g_entries.empty()) return;
        live = g_entries;  // small copy; render outside the lock
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + 24.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##coop_toasts", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    for (const auto& e : live) {
        ImGui::TextColored(e.warn ? ImVec4(1.00f, 0.78f, 0.35f, 1.0f)
                                  : ImVec4(0.92f, 0.95f, 1.00f, 1.0f),
                           "%s", e.text.c_str());
    }
    ImGui::End();
}

}  // namespace ui::toast
