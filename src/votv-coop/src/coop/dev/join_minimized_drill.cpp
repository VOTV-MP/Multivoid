// coop/dev/join_minimized_drill.cpp -- see coop/dev/join_minimized_drill.h.

#include "coop/dev/join_minimized_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/game_window.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/world_load_episode.h"
#include "harness/autotest.h"

#include "ue_wrap/core/log.h"
#include "ui/imgui_overlay.h"
#include "ui/join_curtain.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop::dev::join_minimized_drill {
namespace {

constexpr const char* kTag = "join-minimized";

// A fade of 0.4 s of ImGui time ends within a few frames at any frame rate the rig sees; this many finished
// Render passes without the cover Hidden means the fade is not running, which the drill cannot judge.
constexpr uint64_t kFadePassBound = 600;

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::join_minimized_drill);
    return s;
}

enum class Stage { Minimize, Join, Restore, Fade, Done };

// Game thread, except g_joined (set by the milestone, which any thread may say).
Stage g_stage = Stage::Minimize;
HWND g_minimized = nullptr;  // the window last minimized: a recreated window is minimized again
int g_reasserts = 0;         // times the same window was found restored during the join and minimized again
uint64_t g_fadeFromPass = 0; // the cover's Render count when the fade leg began
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
    namespace V = coop::rig_verdict;
    if (!harness::autotest::IsClientRole()) {
        UE_LOGW("join-minimized: a client-only drill, set on the host: doing nothing");
        g_stage = Stage::Done;
        return;
    }
    const bool presented = ui::imgui_overlay::ImguiReady();
    HWND hwnd = coop::dev::game_window::GameWindow();

    switch (g_stage) {
    case Stage::Minimize:
        if (g_joined.load(std::memory_order_acquire)) {
            Finish(V::Outcome::Invalid, "the join completed before the window was minimized");
            return;
        }
        if (!hwnd) return;  // the window is not there yet: the next tick looks again
        if (presented) {
            Finish(V::Outcome::Invalid, "the window presented a frame before the drill could minimize it");
            return;
        }
        ::ShowWindow(hwnd, SW_MINIMIZE);
        g_minimized = hwnd;
        UE_LOGI("join-minimized: minimized HWND=%p, no frame presented yet", hwnd);
        g_stage = Stage::Join;
        return;

    case Stage::Join: {
        if (presented) {
            Finish(V::Outcome::Invalid,
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
            Finish(V::Outcome::Fail,
                   "the join completed on a window that never presented a frame, but the reconcile window is "
                   "still up: the snapshot-complete case did not finish");
            return;
        }
        const char* why = coop::world_load_episode::LastReconcileLowerReason();
        if (std::strcmp(why, "SnapshotComplete") != 0) {
            char said[160];
            std::snprintf(said, sizeof(said), "the reconcile window was lowered by '%s', not by the snapshot's "
                          "completion, so the case's end is not measured", why);
            Finish(V::Outcome::Invalid, said);
            return;
        }
        if (!coop::join_curtain::IsActive()) {
            Finish(V::Outcome::Invalid, "the join curtain was not up when the join completed");
            return;
        }
        UE_LOGI("join-minimized: the reconcile window lowered with the snapshot and no frame was presented "
                "(re-minimized %d x); restoring HWND=%p to lift the cover", g_reasserts, hwnd);
        if (hwnd) ::ShowWindow(hwnd, SW_RESTORE);
        g_stage = Stage::Restore;
        return;
    }

    case Stage::Restore:
        // The first frame the restored window presents brings the overlay up and runs the cover's Render, which
        // takes the recorded dismissal. Once that pass has finished, a cover still up or still waiting has been
        // given its chance and did not fade: said at once.
        if (coop::join_curtain::RenderPasses() == 0) return;
        if (coop::join_curtain::AwaitingFade()) {
            Finish(V::Outcome::Fail,
                   "the restored window presented a frame and the join curtain did not begin its fade");
            return;
        }
        g_fadeFromPass = coop::join_curtain::RenderPasses();
        g_stage = Stage::Fade;
        return;

    case Stage::Fade:
        // Hidden is the event; the fade's 0.4 s is the overlay's own clock.
        if (!coop::join_curtain::IsActive()) {
            UE_LOGI("join-minimized: the restored window presented and the curtain's fade is complete");
            V::Say(kTag, V::Outcome::Pass,
                   "the join completed on a window that never presented a frame, and the cover lifted once it did");
            UE_LOGI("join-minimized: DONE on client");
            g_stage = Stage::Done;
            return;
        }
        if (coop::join_curtain::RenderPasses() - g_fadeFromPass > kFadePassBound)
            Finish(V::Outcome::Invalid, "the join curtain's fade did not finish within the frame bound");
        return;

    case Stage::Done:
        return;
    }
}

}  // namespace coop::dev::join_minimized_drill
