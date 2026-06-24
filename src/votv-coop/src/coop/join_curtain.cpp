// coop/join_curtain.cpp -- see coop/join_curtain.h. The instant-world SHORT curtain.

#include "coop/join_curtain.h"

#include "imgui.h"

namespace coop::join_curtain {
namespace {

enum class State { Hidden, Up, Fading };
State g_state = State::Hidden;
double g_fadeStart = 0.0;  // ImGui::GetTime() when BeginDismiss() fired

constexpr float kFadeSeconds = 0.40f;  // smooth 1->0 dismiss (not a hard snap); the world fades in assembled

}  // namespace

void Show() {
    g_state = State::Up;
}

void BeginDismiss() {
    if (g_state == State::Up) {       // only from a raised cover; ignore a double-dismiss / not-shown
        g_state = State::Fading;
        g_fadeStart = ImGui::GetTime();
    }
}

void Reset() {
    g_state = State::Hidden;
}

bool IsActive() {
    return g_state != State::Hidden;
}

void Render() {
    if (g_state == State::Hidden) return;

    float alpha = 1.0f;
    if (g_state == State::Fading) {
        const float t = static_cast<float>(ImGui::GetTime() - g_fadeStart) / kFadeSeconds;
        if (t >= 1.0f) { g_state = State::Hidden; return; }   // fade complete -> cover gone
        alpha = 1.0f - (t < 0.0f ? 0.0f : t);
    }

    // Full-viewport black cover on the BACKGROUND draw list: ON TOP of the game world, BEHIND the
    // loading panel (an ImGui window). At alpha=1 the world is fully hidden; during the fade the
    // already-assembled world bleeds through smoothly.
    const ImGuiIO& io = ImGui::GetIO();
    const ImU32 col = IM_COL32(0, 0, 0, static_cast<int>(alpha * 255.0f + 0.5f));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, col);
}

}  // namespace coop::join_curtain
