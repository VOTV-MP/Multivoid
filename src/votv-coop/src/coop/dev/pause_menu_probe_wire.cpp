// coop/dev/pause_menu_probe_wire.cpp -- the pause-menu probe's two-peer reading (M2): the wire counters and the
// world's paused state across a window, and the state the hotkeys consult. See coop/dev/pause_menu_probe.h.

#include "pause_menu_probe_detail.h"

#include "coop/input/input_owner.h"
#include "coop/net/net_stats.h"
#include "ui/input_focus.h"
#include "ui/multiplayer_menu.h"  // IsPauseMenuOpen

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <chrono>

namespace coop::dev::pause_menu_probe::detail::wire {
namespace {

using Clock = std::chrono::steady_clock;
namespace NS = coop::net::net_stats;

struct Window {
    Clock::time_point t0{};
    Clock::time_point lastSec{};
    NS::Snapshot s0{};
    NS::Snapshot last{};
    int sec = 0;
};
Window g_w;

// What the hotkeys' handlers consult, read as they read it. Chat's T (ui/imgui_overlay.cpp:392) needs no overlay
// capture, the pause menu NOT open, the input owner's yes for the key and a chat session; F1 (:360) needs the
// overlay menu open or the input owner's yes.
void LogHotkeys(const char* who) {
    const bool menu = coop::multiplayer_menu::IsPauseMenuOpen();
    const bool f1 = coop::input::input_owner::MayTakeKey(VK_F1);
    const bool t = coop::input::input_owner::MayTakeKey('T');
    UE_LOGI("pmprobe m2 %s hotkeys: pause_menu_open=%d may_take_F1=%d may_take_T=%d overlay_capturing_text=%d "
            "our_window_foreground=%d -> F1 %s, T chat %s", who, menu ? 1 : 0, f1 ? 1 : 0, t ? 1 : 0,
            ui::input_focus::IsOverlayCapturingText() ? 1 : 0, ui::input_focus::IsOurWindowForeground() ? 1 : 0,
            f1 ? "toggles the overlay" : "is left to the game", !menu && t ? "opens" : "does NOT open");
}

}  // namespace

void Begin() {
    NS::Get(g_w.s0);
    g_w.last = g_w.s0;
    g_w.t0 = g_w.lastSec = Clock::now();
    g_w.sec = 0;
}

double Elapsed() { return std::chrono::duration<double>(Clock::now() - g_w.t0).count(); }

// One line a second: wire packets since the last line (every message kind; the pose stream is the steady
// bulk, and no pose-only counter exists), and the world's paused state read by UGameplayStatics::IsGamePaused.
void Pump(const char* who, int maxSeconds) {
    while (Clock::now() - g_w.lastSec >= std::chrono::seconds(1) && g_w.sec < maxSeconds) {
        NS::Snapshot now{};
        NS::Get(now);
        ++g_w.sec;
        UE_LOGI("pmprobe m2 %s t=%ds wire_sent=%llu wire_recv=%llu paused=%d", who, g_w.sec,
                static_cast<unsigned long long>(now.packetsSent - g_w.last.packetsSent),
                static_cast<unsigned long long>(now.packetsRecv - g_w.last.packetsRecv),
                ue_wrap::engine::IsGamePaused() ? 1 : 0);
        if (g_w.sec == 1) LogHotkeys(who);
        g_w.last = now;
        g_w.lastSec += std::chrono::seconds(1);
    }
}

void End(const char* who) {
    NS::Snapshot now{};
    NS::Get(now);
    const double s = Elapsed();
    UE_LOGI("pmprobe m2 %s window=%.1fs wire_sent=%.1f/s wire_recv=%.1f/s paused_at_end=%d", who, s,
            static_cast<double>(now.packetsSent - g_w.s0.packetsSent) / s,
            static_cast<double>(now.packetsRecv - g_w.s0.packetsRecv) / s, ue_wrap::engine::IsGamePaused() ? 1 : 0);
}

}  // namespace coop::dev::pause_menu_probe::detail::wire
