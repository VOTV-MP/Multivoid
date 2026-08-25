// coop/dev/worldless_frames.cpp -- see coop/dev/worldless_frames.h.

#include "coop/dev/worldless_frames.h"

#include "coop/config/config.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <atomic>

namespace coop::dev::worldless_frames {
namespace {

namespace GT  = ue_wrap::game_thread;
namespace WID = ue_wrap::world_identity;

// Armed off the census's row: the two rungs answer one design's questions and there is no
// case for turning one on without the other.
bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::native_ui_probe);
    return s;
}

// =====================================================================================
// The counters. See coop/dev/worldless_frames.h for why the number matters.
// =====================================================================================
//
// Counted on the RENDER thread, one bucket per WorldKind. `Unknown` is the answer we are
// after: it is exactly "CurrentWorld() is null" (a boot window or a travel) plus
// "world_identity is Degraded()", and the report prints Degraded separately so a
// recook-broken chain can never be read as a measurement.
//
// TWO BLIND SPOTS, BOTH MEASURED RATHER THAN ASSUMED AWAY:
//
//  1. The current-world pointer is MEMOISED and refreshed only by a GAME-THREAD caller
//     (world_identity.cpp: `if (IsGameThread()) RefreshOnGameThread_()`), at a 100 ms
//     cadence. At the main menu with no session up, nothing else in the tree calls it, so
//     the memo would sit at its boot value forever and this counter would be measuring
//     the probe's own staleness. So the probe posts its OWN refresh at ~10 Hz while
//     armed. That is a real dependency, and it is why this costs a posted task rather
//     than nothing.
//  2. If the GAME THREAD IS BLOCKED -- a synchronous map load is the case that matters --
//     the posted refresh does not run and the memo goes stale in the exact window the
//     question is about. Rather than pretend, every frame records the AGE of the memo it
//     sampled and frames past the stale ceiling are counted separately. A large `stale`
//     bucket is itself an answer: the game thread was not running, so UMG was not laying
//     anything out either.
constexpr unsigned long long kRefreshPostMs = 100;   // our own game-thread refresh cadence
constexpr unsigned long long kStaleMs       = 300;   // 3x the refresh: an older sample is not trusted
constexpr unsigned long long kReportMs      = 5000;  // periodic report while the picture still changes

std::atomic<unsigned long long> g_lastRefreshMs{0};  // stamped by the refresh task WHEN IT RUNS
std::atomic<unsigned long long> g_lastPostMs{0};     // stamped when we QUEUED it
//
// The two stamps are not the same fact and conflating them would have made the stale
// bucket permanently empty: the post-cadence gate must key on when we ASKED (else a
// blocked game thread gets a queued task every frame, thousands deep), while staleness
// must key on when the task ACTUALLY RAN -- which is exactly what stops happening in
// the window this rung exists to measure.

// Render-thread only (PresentDetour is the sole caller) -- plain scalars, no atomics.
unsigned long long g_frames = 0, g_frUnknown = 0, g_frGameplay = 0, g_frOther = 0, g_frStale = 0;
unsigned long long g_frUnknownFresh = 0;  // the CLEAN half of unknown -- see below
unsigned long long g_runUnknown = 0, g_maxRunUnknown = 0, g_maxRunUnknownFresh = 0;
unsigned long long g_runUnknownFresh = 0;
unsigned long long g_reportedFrames = 0;

// `unknown` and `unknownFresh` are TWO DIFFERENT ANSWERS and the first run of this probe
// conflated them: 239 of 504 frames read Unknown, and all 239 were also STALE -- because
// they were presented before the game thread had run a single one of our refresh tasks,
// so the memo had never been written at all. "No world existed" and "we had not yet
// measured whether one existed" are not the same claim, and only the first one is
// evidence about what UMG could have drawn. `unknownFresh` is the frames that read
// Unknown from a sample that was actually current; that is the number O4 turns on.
void ReportRung0(const char* why) {
    UE_LOGI("[native_ui_probe] RUNG0 %s: frames=%llu  unknown=%llu (fresh %llu, max run %llu/"
            "%llu)  gameplay=%llu  other=%llu  stale=%llu  degraded=%d",
            why, g_frames, g_frUnknown, g_frUnknownFresh, g_maxRunUnknown, g_maxRunUnknownFresh,
            g_frGameplay, g_frOther, g_frStale, WID::Degraded() ? 1 : 0);
}

}  // namespace

void NoteFrame() {
    if (!Armed()) return;
    const unsigned long long now = ::GetTickCount64();
    // Own the refresh (see the RUNG 0 block header): at the main menu with no session up
    // nothing else on the game thread calls CurrentWorld(), so without this the counter
    // would be measuring our own staleness instead of the game's world.
    if (now - g_lastPostMs.load(std::memory_order_relaxed) >= kRefreshPostMs) {
        g_lastPostMs.store(now, std::memory_order_relaxed);
        GT::Post([] {
            (void)WID::CurrentWorldKind();
            g_lastRefreshMs.store(::GetTickCount64(), std::memory_order_relaxed);
        });
    }
    ++g_frames;
    // Staleness is measured against the RAN stamp, never the QUEUED one. A stamp of 0
    // means no refresh has EVER landed -- the boot window -- which is stale by the same
    // rule and needs no special case.
    const unsigned long long ran = g_lastRefreshMs.load(std::memory_order_relaxed);
    const bool stale = (ran == 0) || (now - ran > kStaleMs);
    if (stale) ++g_frStale;
    switch (WID::CurrentWorldKind()) {
        case WID::WorldKind::Unknown:
            ++g_frUnknown;
            if (++g_runUnknown > g_maxRunUnknown) g_maxRunUnknown = g_runUnknown;
            if (!stale) {
                ++g_frUnknownFresh;
                if (++g_runUnknownFresh > g_maxRunUnknownFresh)
                    g_maxRunUnknownFresh = g_runUnknownFresh;
            } else {
                g_runUnknownFresh = 0;
            }
            break;
        case WID::WorldKind::Gameplay:
            ++g_frGameplay; g_runUnknown = 0; g_runUnknownFresh = 0; break;
        default:
            ++g_frOther;    g_runUnknown = 0; g_runUnknownFresh = 0; break;
    }
    static unsigned long long sNextReport = 0;
    if (now >= sNextReport) {
        sNextReport = now + kReportMs;
        if (g_frames != g_reportedFrames) {  // stay quiet when nothing is presenting
            g_reportedFrames = g_frames;
            ReportRung0("periodic");
        }
    }
}

}  // namespace coop::dev::worldless_frames
