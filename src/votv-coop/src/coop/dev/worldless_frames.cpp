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
// A FRAME LANDS IN EXACTLY ONE OF THREE CONFIDENCE CLASSES, and the first version of this
// file had only two -- which made its headline number unattributable:
//
//   PUMP-FROZEN  `GT::TasksRun()` has not ADVANCED for longer than the ceiling: the game
//             thread is presenting frames but is not draining our task queue. This is the
//             decisive class and it took two runs to find. The first version asked
//             `TasksRun() == 0` ("has the pump EVER run"), which is false almost
//             immediately -- boot posts tasks before the first present -- so the bucket
//             read 0 while 478 frames sat stale. HAS-EVER-RUN and IS-RUNNING-NOW are
//             different questions and only the second one is about a window.
//   STALE     the pump is advancing but our own refresh has not landed inside the ceiling.
//             Rare; if it dominates, the post cadence is wrong, not the game.
//   FRESH     the pump is current. Only these frames are evidence about the world.
//
// WHY PUMP-FROZEN IS THE ANSWER TO O4 AND THE WORLD MEMO IS NOT. Every UFunction this mod
// calls -- `SpawnObject` included -- must run on the game thread, and it gets there through
// `GT::Post`, which drains inside our ProcessEvent detour. So in a PUMP-FROZEN window we
// cannot CREATE or DRIVE a UMG widget at all, whatever the engine is doing with Slate
// meanwhile. That makes the frozen-window frame count a direct measurement of the window a
// UMG surface structurally cannot serve, with no dependency on the world memo -- which is
// itself unreadable in exactly that window, and unreadable for a reason that is about OUR
// substrate rather than about UMG.
//
// TWO DEPENDENCIES, both real and both stated rather than assumed away:
//
//  1. The current-world pointer is MEMOISED and refreshed only by a GAME-THREAD caller
//     (world_identity.cpp: `if (IsGameThread()) RefreshOnGameThread_()`), at a 100 ms
//     cadence. The probe posts its OWN refresh at ~10 Hz while armed, and stamps WHEN THAT
//     TASK RAN -- which is what makes the STALE class a measurement instead of an
//     assumption. Without a stamp of our own there is no way to say how old the sample
//     backing a given frame actually was.
//
//     CORRECTED 2026-08-26: the first version of this note justified the refresh by
//     claiming that "at the main menu with no session up, nothing else in the tree calls
//     it, so the memo would sit at its boot value forever". THAT IS FALSE, and it is worth
//     leaving the correction here because a critic reading it built a whole question on it.
//     `imgui_overlay.cpp:541-548` posts `input_owner::TickGameThread` every 100 ms from
//     PresentDetour, and that function's FIRST statement is `CurrentWorld()` under a
//     comment naming itself "THE REFRESH FLOOR ... 10 Hz, ungated, game thread, alive at
//     the menu with no session" (input_owner.cpp:294-312). So the memo IS warm here; the
//     probe's refresh is redundant AS A REFRESH and is kept only for the stamp above.
//  2. `GT::Post` tasks drain inside our ProcessEvent detour, so the pump needs both the
//     detour installed AND ProcessEvent traffic. The detour installs early, but BP dispatch
//     traffic is near zero during the boot load -- which is exactly what the PUMP-FROZEN
//     class exists to name, and why the game can present at ~42 fps while our pump does not
//     advance at all.
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
unsigned long long g_frames = 0, g_frUnknown = 0, g_frGameplay = 0, g_frOther = 0;
unsigned long long g_frPumpFrozen = 0, g_frStale = 0, g_frFresh = 0;
// Pump-liveness tracking: the VALUE plus when it last CHANGED.
unsigned long long g_lastTasksRun = 0, g_lastTasksAdvanceMs = 0;
unsigned long long g_maxFrozenFrames = 0, g_runFrozen = 0, g_maxFrozenMs = 0;
unsigned long long g_frUnknownFresh = 0;   // the only frames that are evidence about the world
unsigned long long g_runUnknown = 0, g_maxRunUnknown = 0;
unsigned long long g_runUnknownFresh = 0, g_maxRunUnknownFresh = 0;
unsigned long long g_reportedFrames = 0;

// PER-SEGMENT counters, reset at every WorldKind CHANGE. A running total cannot answer
// "how many frames were presented DURING the travel" -- by the time the total is printed
// the travel is over and its frames are indistinguishable from the boot window's. The
// edge line is therefore the primary output of this rung and the totals are the summary.
WID::WorldKind g_segKind = WID::WorldKind::Unknown;
bool g_segStarted = false;
unsigned long long g_segFrames = 0, g_segFresh = 0, g_segStale = 0, g_segFrozen = 0;
unsigned long long g_segStartMs = 0;

const char* KindName(WID::WorldKind k) {
    switch (k) {
        case WID::WorldKind::Gameplay: return "Gameplay";
        case WID::WorldKind::Other:    return "Other";
        default:                       return "Unknown";
    }
}

void ReportRung0(const char* why) {
    UE_LOGI("[native_ui_probe] RUNG0 %s: frames=%llu | kinds unknown=%llu gameplay=%llu other=%llu "
            "| PUMP-FROZEN frames=%llu (longest run %llu frames / %llu ms) stale=%llu fresh=%llu "
            "| UNKNOWN-AND-FRESH=%llu (max run %llu, vs %llu counting all) | tasksRun=%llu degraded=%d",
            why, g_frames, g_frUnknown, g_frGameplay, g_frOther, g_frPumpFrozen, g_maxFrozenFrames,
            g_maxFrozenMs, g_frStale, g_frFresh, g_frUnknownFresh, g_maxRunUnknownFresh,
            g_maxRunUnknown, GT::TasksRun(), WID::Degraded() ? 1 : 0);
}

}  // namespace

void NoteFrame() {
    if (!Armed()) return;
    const unsigned long long now = ::GetTickCount64();
    // Own the refresh (see the block header): at the main menu with no session up nothing
    // else on the game thread calls CurrentWorld(), so without this the counter would be
    // measuring our own staleness instead of the game's world.
    if (now - g_lastPostMs.load(std::memory_order_relaxed) >= kRefreshPostMs) {
        g_lastPostMs.store(now, std::memory_order_relaxed);
        GT::Post([] {
            (void)WID::CurrentWorldKind();
            g_lastRefreshMs.store(::GetTickCount64(), std::memory_order_relaxed);
        });
    }
    ++g_frames;

    // Confidence class. PUMP-FROZEN is asked FIRST and subsumes staleness: a frozen pump
    // is WHY the refresh is old, and reporting the symptom instead of the cause is what
    // made the first two runs unreadable.
    const unsigned long long tasks = GT::TasksRun();
    if (tasks != g_lastTasksRun) { g_lastTasksRun = tasks; g_lastTasksAdvanceMs = now; }
    // A zero advance-stamp means we have never seen the pump move; treat the process
    // start as the reference so the very first frames are not falsely "frozen for 51 days".
    if (g_lastTasksAdvanceMs == 0) g_lastTasksAdvanceMs = now;
    const unsigned long long frozenMs = now - g_lastTasksAdvanceMs;
    const bool pumpFrozen = frozenMs > kStaleMs;
    const unsigned long long ran = g_lastRefreshMs.load(std::memory_order_relaxed);
    const bool stale = !pumpFrozen && ((ran == 0) || (now - ran > kStaleMs));
    const bool fresh = !pumpFrozen && !stale;
    if (pumpFrozen) {
        ++g_frPumpFrozen;
        if (++g_runFrozen > g_maxFrozenFrames) g_maxFrozenFrames = g_runFrozen;
        if (frozenMs > g_maxFrozenMs) g_maxFrozenMs = frozenMs;
    } else {
        g_runFrozen = 0;
        if (stale) ++g_frStale; else ++g_frFresh;
    }

    const WID::WorldKind kind = WID::CurrentWorldKind();
    switch (kind) {
        case WID::WorldKind::Unknown:
            ++g_frUnknown;
            if (++g_runUnknown > g_maxRunUnknown) g_maxRunUnknown = g_runUnknown;
            if (fresh) {
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

    // The EDGE line -- the primary output. Emitted when the kind changes, carrying the
    // segment that just ENDED, so a travel's cost is readable as its own number instead
    // of being folded into a running total after the fact.
    if (!g_segStarted) {
        g_segStarted = true;
        g_segKind = kind;
        g_segStartMs = now;
    } else if (kind != g_segKind) {
        UE_LOGW("[native_ui_probe] RUNG0 EDGE %s -> %s after %llu frames / %llu ms in %s "
                "(pumpFrozen %llu, stale %llu, fresh %llu)",
                KindName(g_segKind), KindName(kind), g_segFrames, now - g_segStartMs,
                KindName(g_segKind), g_segFrozen, g_segStale, g_segFresh);
        g_segKind = kind;
        g_segStartMs = now;
        g_segFrames = g_segFresh = g_segStale = g_segFrozen = 0;
    }
    ++g_segFrames;
    if (pumpFrozen) ++g_segFrozen; else if (stale) ++g_segStale; else ++g_segFresh;

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
