// ui/join_curtain.cpp -- see ui/join_curtain.h. The instant-world SHORT curtain.

#include "ui/join_curtain.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "imgui.h"

#include <atomic>

namespace coop::join_curtain {
namespace {

// State is WRITTEN on the game thread (Show/BeginDismiss/Reset are called from the event_feed net-drain /
// join lifecycle) and READ on the RENDER thread (Render is the DX Present hook) -- so it is atomic, exactly
// like join_progress's phase. BeginDismiss only RECORDS the dismissal (Up -> DismissRequested): the fade clock
// is ImGui's, and the game thread has no ImGui context before the first presented frame, so Render, inside a
// frame, owns the two transitions that read it: DismissRequested -> Fading (it stamps g_fadeStart first, then
// publishes the state with release) and Fading -> Hidden once the fade is done. Both are compare-exchanges: a
// Show or Reset (plain stores, game thread) between Render's read and its exchange makes the exchange fail,
// and the newer state stands.
enum class State { Hidden, Up, DismissRequested, Fading };
std::atomic<int>    g_state{static_cast<int>(State::Hidden)};
std::atomic<double> g_fadeStart{0.0};  // ImGui::GetTime() when Render took the dismissal

constexpr float kFadeSeconds = 0.40f;  // smooth 1->0 dismiss (not a hard snap); the world fades in assembled

State Get() { return static_cast<State>(g_state.load(std::memory_order_acquire)); }

}  // namespace

void Show() {
    g_state.store(static_cast<int>(State::Up), std::memory_order_release);
}

void BeginDismiss() {
    // Only from a raised cover; ignore a double-dismiss / not-shown. No ImGui call here: this runs on the game
    // thread, where a window that has never presented has no context (ImGui::GetTime dereferences the null
    // context and faults the whole SnapshotComplete case). Render stamps the fade's start when it takes the
    // request.
    static const bool sClockRed =
        coop::config::ResolveFlag(::coop::config_registry::rows::curtain_dismiss_frame_clock_off);
    int expected = static_cast<int>(State::Up);
    if (sClockRed) {
        // Drill red: the clock is read here, outside any frame, as the build before the fix did.
        g_fadeStart.store(ImGui::GetTime(), std::memory_order_relaxed);
        g_state.compare_exchange_strong(expected, static_cast<int>(State::Fading),
                                        std::memory_order_release, std::memory_order_relaxed);
        return;
    }
    g_state.compare_exchange_strong(expected, static_cast<int>(State::DismissRequested),
                                    std::memory_order_release, std::memory_order_relaxed);
}

void Reset() {
    g_state.store(static_cast<int>(State::Hidden), std::memory_order_release);
}

bool IsActive() {
    return Get() != State::Hidden;
}

void Render() {
    State st = Get();
    if (st == State::Hidden) return;

    if (st == State::DismissRequested) {
        // Inside a frame, so the context exists. The start is stored before the state is published.
        g_fadeStart.store(ImGui::GetTime(), std::memory_order_relaxed);
        int expected = static_cast<int>(State::DismissRequested);
        g_state.compare_exchange_strong(expected, static_cast<int>(State::Fading),
                                        std::memory_order_release, std::memory_order_relaxed);
        st = Get();  // Fading, or what a Show/Reset made of it in between
        if (st == State::Hidden) return;
    }

    float alpha = 1.0f;
    if (st == State::Fading) {
        const double start = g_fadeStart.load(std::memory_order_relaxed);
        const float t = static_cast<float>(ImGui::GetTime() - start) / kFadeSeconds;
        if (t >= 1.0f) {
            // Fade complete: nothing to draw, and the cover is no longer active, so the overlay stops being
            // asked for a frame on its account. A Show/Reset since the read wins the exchange.
            int expected = static_cast<int>(State::Fading);
            g_state.compare_exchange_strong(expected, static_cast<int>(State::Hidden),
                                            std::memory_order_release, std::memory_order_relaxed);
            return;
        }
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
