// coop/dev/join_minimized_drill.cpp -- see coop/dev/join_minimized_drill.h.

#include "coop/dev/join_minimized_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/game_window.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/core/log.h"
#include "ui/imgui_overlay.h"
#include "ui/join_curtain.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace coop::dev::join_minimized_drill {
namespace {

constexpr const char* kTag = "join-minimized";

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::join_minimized_drill);
    return s;
}

enum class Stage { Minimize, Join, Restore, Done };

// Game thread, except g_joined (set by the milestone, which any thread may say).
Stage g_stage = Stage::Minimize;
HWND g_minimized = nullptr;  // the window last minimized: a recreated window is minimized again
int g_reasserts = 0;         // times the same window was found restored during the join and minimized again
std::atomic<bool> g_joined{false};

void Finish(coop::rig_verdict::Outcome outcome, const char* why) {
    coop::rig_verdict::Say(kTag, outcome, "%s", why);
    g_stage = Stage::Done;
}

}  // namespace

void OnMilestone(const char* milestone) {
    if (!IsEnabled() || !milestone || std::strcmp(milestone, "joined") != 0) return;
    g_joined.store(true, std::memory_order_release);
}

void OnFrameTail() {
    if (!IsEnabled() || g_stage == Stage::Done) return;
    const bool presented = ui::imgui_overlay::ImguiReady();
    HWND hwnd = coop::dev::game_window::GameWindow();

    switch (g_stage) {
    case Stage::Minimize:
        if (g_joined.load(std::memory_order_acquire)) {
            Finish(coop::rig_verdict::Outcome::Invalid, "the join completed before the window was minimized");
            return;
        }
        if (!hwnd) return;  // the window is not there yet: the next tick looks again
        if (presented) {
            Finish(coop::rig_verdict::Outcome::Invalid,
                   "the window presented a frame before the drill could minimize it");
            return;
        }
        ::ShowWindow(hwnd, SW_MINIMIZE);
        g_minimized = hwnd;
        UE_LOGI("join-minimized: minimized HWND=%p, no frame presented yet", hwnd);
        g_stage = Stage::Join;
        return;

    case Stage::Join:
        if (presented) {
            Finish(coop::rig_verdict::Outcome::Invalid,
                   "the window presented a frame before the join completed, so the minimize did not take");
            return;
        }
        if (!g_joined.load(std::memory_order_acquire)) {
            // Hold it minimized: a window the engine recreated, or restored, is minimized again.
            if (hwnd && (hwnd != g_minimized || !::IsIconic(hwnd))) {
                if (hwnd == g_minimized) ++g_reasserts;
                else UE_LOGI("join-minimized: the window was recreated, minimizing HWND=%p", hwnd);
                ::ShowWindow(hwnd, SW_MINIMIZE);
                g_minimized = hwnd;
            }
            return;
        }
        // The first game-thread tick after join_progress::Complete: the SnapshotComplete case has run to its
        // end, or faulted and skipped the rest, and left the window up.
        if (coop::world_load_episode::InReconcileWindow()) {
            Finish(coop::rig_verdict::Outcome::Fail,
                   "the join completed on a window that never presented a frame, but the reconcile window is "
                   "still up: the snapshot-complete case did not finish");
            return;
        }
        UE_LOGI("join-minimized: the reconcile window is down, no frame was ever presented (re-minimized %d x)",
                g_reasserts);
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass,
                               "the join completed and the reconcile window lowered on a window that never "
                               "presented a frame");
        UE_LOGI("join-minimized: restoring HWND=%p, curtain active=%d", hwnd,
                coop::join_curtain::IsActive() ? 1 : 0);
        if (hwnd) ::ShowWindow(hwnd, SW_RESTORE);
        g_stage = Stage::Restore;
        return;

    case Stage::Restore:
        // The first presented frame brings the overlay up, whose Render takes the recorded dismissal into the
        // fade and ends it: the curtain is no longer active once that has run.
        if (presented && !coop::join_curtain::IsActive()) {
            UE_LOGI("join-minimized: the restored window presented and the curtain's fade is complete");
            UE_LOGI("join-minimized: DONE on client");
            g_stage = Stage::Done;
        }
        return;

    case Stage::Done:
        return;
    }
}

}  // namespace coop::dev::join_minimized_drill
