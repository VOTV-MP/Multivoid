// ui/console.cpp -- see ui/console.h.

#include "ui/console.h"

#include "coop/session/join_progress.h"
#include "ue_wrap/log.h"

#include "imgui.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace ui::console {
namespace {

namespace jp = coop::join_progress;
using ue_wrap::log::Level;

// Fixed-size ring of fixed-size lines: the logger sink writes into it on whatever thread
// logged (including hot paths), so it must NOT allocate per line. A short mutex around a
// memcpy is the whole cost (no std::string churn). 512 lines is plenty for a connect log.
struct CLine {
    Level level = Level::Info;
    char  text[200] = "";
};
constexpr int kRingCap = 512;
CLine     g_ring[kRingCap];
uint32_t  g_count = 0;          // total lines ever written; newest kRingCap are live
std::mutex g_mu;               // guards g_ring + g_count

std::atomic<bool> g_userOpen{false};   // user toggle (auto-show during a join is separate)
// Set by OnLog (any logging thread) + consumed by Render (render thread) -> atomic, not a
// plain bool (audit HIGH-1: a non-atomic cross-thread bool is a data race / UB).
std::atomic<bool> g_scrollToBottom{true};
char g_input[256] = "";

// The logger sink. Called from ue_wrap::log::Write OUTSIDE its critical section, on the
// logging thread. Cheap + non-allocating + must not log (no recursion).
void OnLog(Level level, const char* msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    CLine& l = g_ring[g_count % kRingCap];
    l.level = level;
    size_t n = 0;
    for (; msg[n] && n < sizeof(l.text) - 1; ++n) l.text[n] = msg[n];
    l.text[n] = '\0';
    ++g_count;
    g_scrollToBottom.store(true, std::memory_order_relaxed);
}

ImVec4 ColorFor(Level l) {
    switch (l) {
        case Level::Warn:  return ImVec4(0.96f, 0.80f, 0.35f, 1.0f);
        case Level::Error: return ImVec4(0.97f, 0.45f, 0.45f, 1.0f);
        default:           return ImVec4(0.78f, 0.81f, 0.85f, 1.0f);
    }
}

// Minimal LOCAL command handling. Echoes + dispatches a typed line. Networked host/client
// commands arrive with the command subsystem; for now: help, clear. Logs via UE_LOGI so the
// echo flows back through the sink into the log view (and the file log).
void RunCommand(const char* cmd) {
    while (*cmd == ' ') ++cmd;
    if (*cmd == '\0') return;
    UE_LOGI("console> %s", cmd);
    if (std::strcmp(cmd, "clear") == 0) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_count = 0;
        return;
    }
    if (std::strcmp(cmd, "help") == 0) {
        UE_LOGI("console: commands -- help, clear. (host/client game commands coming soon)");
        return;
    }
    UE_LOGI("console: unknown command '%s' (try 'help')", cmd);
}

}  // namespace

void Init() { ue_wrap::log::SetSink(&OnLog); }
void Shutdown() { ue_wrap::log::SetSink(nullptr); }

bool IsOpen() { return g_userOpen.load(std::memory_order_relaxed) || jp::Active(); }
void Open()  { g_userOpen.store(true, std::memory_order_relaxed); }
void Close() { g_userOpen.store(false, std::memory_order_relaxed); }
void Toggle(){ g_userOpen.store(!g_userOpen.load(std::memory_order_relaxed), std::memory_order_relaxed); }

void Render() {
    if (!IsOpen()) return;

    const ImGuiIO& io = ImGui::GetIO();
    // Bottom-anchored panel, wide + short, semi-transparent -- a Quake-style drop console.
    const float w = io.DisplaySize.x * 0.56f;
    const float h = 260.0f;
    ImGui::SetNextWindowPos(ImVec2(16.0f, io.DisplaySize.y - h - 16.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.06f, 0.88f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Console###coop_console", nullptr, flags)) {
        // Log scroll region (reserve a line for the input box).
        const float inputH = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##log", ImVec2(0.0f, -inputH), false,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            // Snapshot the live lines under the lock, then render without it (so the sink
            // never blocks on ImGui). Copy is bounded (<= kRingCap short lines).
            static CLine snap[kRingCap];
            int n = 0;
            uint32_t startIdx = 0;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                const uint32_t total = g_count;
                const uint32_t live = total < static_cast<uint32_t>(kRingCap)
                                          ? total : static_cast<uint32_t>(kRingCap);
                startIdx = total - live;
                for (uint32_t i = 0; i < live; ++i) snap[n++] = g_ring[(startIdx + i) % kRingCap];
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
            for (int i = 0; i < n; ++i) {
                ImGui::PushStyleColor(ImGuiCol_Text, ColorFor(snap[i].level));
                ImGui::TextUnformatted(snap[i].text);
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();
            if (g_scrollToBottom.load(std::memory_order_relaxed) &&
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            g_scrollToBottom.store(false, std::memory_order_relaxed);
        }
        ImGui::EndChild();

        // Command input.
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##cmd", "type a command -- 'help'", g_input, sizeof(g_input),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            RunCommand(g_input);
            g_input[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);  // keep focus for the next command
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

}  // namespace ui::console
