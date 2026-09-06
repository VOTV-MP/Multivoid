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

// The counters. See coop/dev/worldless_frames.h for why the number matters.
//
// Counted on the RENDER thread, one bucket per WorldKind. `Unknown` is the answer we are after:
// exactly "CurrentWorld() is null" (a boot window or a travel) plus "world_identity is
// Degraded()", and the report prints Degraded separately so a recook-broken chain can never be
// read as a measurement.
//
// A frame lands in one of three confidence classes. PUMP-FROZEN: `GT::TasksRun()` has not
// ADVANCED for longer than the ceiling, so the game thread presents frames without draining our
// task queue -- the test is whether the pump runs NOW, since boot posts tasks before the first
// present and "has it ever run" is true almost immediately. STALE: the pump advances but our own
// refresh has not landed inside the ceiling; if that dominates, the post cadence is wrong, not
// the game. FRESH: the pump is current, and only these frames are evidence about the world.
constexpr unsigned long long kRefreshPostMs = 100;   // our own game-thread refresh cadence
constexpr unsigned long long kStaleMs       = 300;   // 3x the refresh: an older sample is not trusted
constexpr unsigned long long kReportMs      = 5000;  // periodic report while the picture still changes
// PUMP-FROZEN, not the world memo, is what says whether a UMG surface could serve a window.
// Every UFunction this mod calls, `SpawnObject` included, reaches the game thread through
// `GT::Post`, which drains inside our ProcessEvent detour, so in a frozen window we cannot
// create or drive a UMG widget at all, whatever the engine is doing with Slate. The pump needs
// both the detour installed AND ProcessEvent traffic: the detour installs early, but blueprint
// dispatch traffic is near zero during the boot load, which is why the game can present at
// ~42 fps while our pump does not advance at all.

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
    // Post our own refresh -- not because the memo would go cold (input_owner's 10 Hz tick
    // already calls CurrentWorld() from this same present path) but to STAMP when a
    // game-thread task actually RAN. Without that stamp there is no way to say how old the
    // sample behind a frame was, and the STALE class would be an assumption.
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
