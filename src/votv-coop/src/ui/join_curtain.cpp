// ui/join_curtain.cpp -- see ui/join_curtain.h. The instant-world SHORT curtain.

#include "ui/join_curtain.h"

#include "imgui.h"

#include <atomic>

namespace coop::join_curtain {
namespace {

// State is WRITTEN on the game thread (Show/BeginDismiss/Reset are called from the event_feed net-drain /
// join lifecycle) and READ on the RENDER thread (Render is the DX Present hook) -- so it is atomic, exactly
// like join_progress's phase. Single game-thread writer + render reader (Render is READ-ONLY: it never
// mutates g_state -- a completed fade just draws nothing; the next Show() resets it). g_fadeStart is published
// by BeginDismiss (game) and read by Render (render).
enum class State { Hidden, Up, Fading };
std::atomic<int>    g_state{static_cast<int>(State::Hidden)};
// ImGui::GetTime() when the render thread first drew the fade; kUnstamped until it did. The game thread never reads the ImGui
// clock: ImGui::GetTime() dereferences the current context, which does not exist until the overlay's first frame, and a
// fault there (seen on a client whose join completed before its first Present) skipped the rest of the SnapshotComplete
// handler: the reconcile window never lowered and the host never heard the client ready.
constexpr double kUnstamped = -1.0;
std::atomic<double> g_fadeStart{kUnstamped};

constexpr float kFadeSeconds = 0.40f;  // smooth 1->0 dismiss (not a hard snap); the world fades in assembled

State Get() { return static_cast<State>(g_state.load(std::memory_order_acquire)); }

}  // namespace

void Show() {
    g_state.store(static_cast<int>(State::Up), std::memory_order_release);
}

void BeginDismiss() {
    // Only from a raised cover; ignore a double-dismiss / not-shown. Clear the start stamp BEFORE flipping to Fading so the
    // render thread, on observing Fading (acquire), stamps the fade's start itself with its own clock (release pairs).
    int expected = static_cast<int>(State::Up);
    g_fadeStart.store(kUnstamped, std::memory_order_relaxed);
    g_state.compare_exchange_strong(expected, static_cast<int>(State::Fading),
                                    std::memory_order_release, std::memory_order_relaxed);
}

void Reset() {
    g_state.store(static_cast<int>(State::Hidden), std::memory_order_release);
}

bool IsActive() {
    return Get() != State::Hidden;
}

void Render() {
    const State st = Get();
    if (st == State::Hidden) return;

    float alpha = 1.0f;
    if (st == State::Fading) {
        double start = g_fadeStart.load(std::memory_order_relaxed);
        if (start == kUnstamped) {   // the first frame of the fade: Render is the only writer from here
            start = ImGui::GetTime();
            g_fadeStart.store(start, std::memory_order_relaxed);
        }
        const float t = static_cast<float>(ImGui::GetTime() - start) / kFadeSeconds;
        if (t >= 1.0f) return;  // fade complete -> draw nothing (Render is read-only; state stays Fading
                                // until the next Show()/Reset() on the game thread -- harmless, IsActive
                                // is not load-bearing).
        alpha = 1.0f - (t < 0.0f ? 0.0f : t);
    }

    // Full-viewport black cover on the BACKGROUND draw list: ON TOP of the game world, BEHIND the loading
    // panel (an ImGui window). At alpha=1 the world is fully hidden; during the fade the already-assembled
    // world bleeds through smoothly.
    const ImGuiIO& io = ImGui::GetIO();
    const ImU32 col = IM_COL32(0, 0, 0, static_cast<int>(alpha * 255.0f + 0.5f));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, col);
}

}  // namespace coop::join_curtain
