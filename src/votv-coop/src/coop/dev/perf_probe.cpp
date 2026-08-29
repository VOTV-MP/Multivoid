// coop/dev/perf_probe.cpp -- see coop/dev/perf_probe.h.
//
// MEASURE-first probe for the 15-FPS audit. Owns the frame counter + the per-
// subsystem Tick buckets; reads the detour's dispatch/self-time/observer-body
// counters out of ue_wrap::game_thread (which owns them because it is the lower
// layer the detour lives in). Once a second it logs the rates + per-frame costs.

#include "coop/dev/perf_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>
#include <psapi.h>  // PROCESS_MEMORY_COUNTERS_EX + K32GetProcessMemoryInfo (kernel32 export; no psapi.lib link)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <string>

namespace coop::dev::perf_probe {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R = ue_wrap::reflection;

bool  g_armed     = false;   // set by Init() from the ini; read by Armed()
bool  g_selfTime  = false;
bool  g_initDone  = false;

// Frame counter (incremented from the ImGui Present detour) + subsystem buckets.
std::atomic<unsigned long long> g_frames{0};
std::array<std::atomic<unsigned long long>, static_cast<size_t>(Bucket::Count)> g_buckets{};

const char* kBucketNames[static_cast<size_t>(Bucket::Count)] = {
    "netPumpTick", "reaper", "localSend", "installObs", "interactable", "weatherConn",
    "itemConn", "trashWatch", "balance", "snapshotDrain", "remoteProp", "puppets",
    "eventFeed", "nameplate", "roster", "overlayPresent",
};

long long QpcFreq() {
    static long long s_freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    return s_freq;
}

double TicksToMs(unsigned long long ticks) {
    const long long f = QpcFreq();
    return f > 0 ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(f) : 0.0;
}

// ---- 1 Hz sampler window state (game-thread only; Sample() runs there) --------
std::chrono::steady_clock::time_point g_lastSample{};
bool g_haveBaseline = false;
unsigned long long g_lastPE = 0, g_lastPEGT = 0, g_lastSelfNs = 0, g_lastSelfSamp = 0,
                   g_lastObsNs = 0, g_lastFrames = 0;
// Whole-detour window state (2026-08-29); see the WHOLE readout in Sample().
unsigned long long g_lastWholeNs = 0, g_lastEngineNs = 0, g_lastWholeSamp = 0, g_lastTopLevel = 0;
std::array<unsigned long long, static_cast<size_t>(Bucket::Count)> g_lastBuckets{};

}  // namespace

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::perf_probe);
    return s;
}

bool Armed() { return g_armed; }

unsigned long long NowTicks() {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return static_cast<unsigned long long>(t.QuadPart);
}

void AddTicks(Bucket b, unsigned long long ticks) {
    if (!g_armed) return;
    const size_t i = static_cast<size_t>(b);
    if (i < g_buckets.size()) g_buckets[i].fetch_add(ticks, std::memory_order_relaxed);
}

Scope::Scope(Bucket b) : b_(b), on_(g_armed), t0_(0) {
    if (on_) t0_ = NowTicks();
}
Scope::~Scope() {
    if (on_) AddTicks(b_, NowTicks() - t0_);
}

void Init() {
    if (g_initDone) return;
    g_initDone = true;
    if (!Enabled()) return;
    g_armed = true;
    g_selfTime = coop::config::ResolveFlag(::coop::config_registry::rows::perf_probe_selftime);
    GT::SetPerfCounting(true, g_selfTime);
    UE_LOGW("[perf] probe ARMED (perf_probe=1, selftime=%d) -- 1 Hz frame-cost report follows; "
            "this adds per-dispatch counting overhead, turn OFF for real play", g_selfTime ? 1 : 0);
}

void NoteFrame() {
    // BOTH Init() and Sample() are driven from net_pump::Tick, which does not run
    // outside a coop session -- so the probe produced NO data at all for "mod loaded,
    // not hosting", which is exactly the baseline needed to split the DLL's resident
    // cost from the coop session's (measured 2026-08-29: 120 fps with no mod vs 75 fps
    // merely hosting, while every instrumented bucket summed to ~0.6 ms/frame).
    //
    // Init must be posted too, not just Sample: Init is the ONLY thing that sets
    // g_armed, so gating this on g_armed first -- as this function did until the fix --
    // means the probe can never arm without a session and the baseline stays
    // unmeasurable. Init self-latches and Sample self-throttles to ~1 Hz, so this is at
    // most one posted task per second and a no-op whenever net_pump already sampled.
    static ULONGLONG sNextPost = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now >= sNextPost) {
        sNextPost = now + 1000;
        ue_wrap::game_thread::Post([] { Init(); Sample(); });
    }
    if (!g_armed) return;
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

void Sample() {
    if (!g_armed) return;
    const auto now = std::chrono::steady_clock::now();
    if (!g_haveBaseline) {
        g_haveBaseline = true;
        g_lastSample = now;
        return;  // establish the first baseline; report from the next window
    }
    const double elapsed = std::chrono::duration<double>(now - g_lastSample).count();
    if (elapsed < 1.0) return;
    g_lastSample = now;

    // RAM-balloon instrument (2026-06-13): the client slowly balloons to an OOM
    // safety-kill over ~10 min with NO log signature (off-game-thread heap leak;
    // all our bounded containers ruled out). This 1 Hz line PROVES the climb +
    // rate so the next test localizes the source. PrivateUsage (commit) is the
    // balloon indicator -- it grows with leaked heap regardless of working-set
    // trimming. K32GetProcessMemoryInfo is a kernel32 export (no psapi.lib link).
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            UE_LOGW("[perf][mem] private(commit)=%.1f MB workingset=%.1f MB -- watch for steady climb (RAM-balloon hunt)",
                    pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0);
        }
    }

    // One-shot `stat unit`. Our own buckets can only ever account for OUR code; they
    // cannot say whether a lost millisecond went to the game thread, the render thread
    // or the GPU -- and on 2026-08-29 that was exactly the open question (120 fps
    // without the mod vs ~68 with it, while every bucket we own summed to ~1 ms). UE4's
    // own unit graph splits the frame three ways and is the only thing that can point
    // at the right half of the engine. Deferred to Sample() rather than Init() because
    // it needs a world; retried until the call reports success.
    {
        static bool sStatUnitOn = false;
        if (!sStatUnitOn && ue_wrap::engine::ExecuteConsoleCommand(L"stat unit")) {
            sStatUnitOn = true;
            // UNCAP FIRST, or every number above is a reading of the cap and not of the
            // workload. Measured 2026-08-29: host and client both reported Frame=16.65 ms
            // -- identical to a hundredth of a millisecond across two independent
            // processes, i.e. 60.06 Hz -- with Game 16.13/16.20 and GPU 16.53/16.73. Under
            // vsync UE4's game thread BLOCKS on the frame sync and `stat unit` counts that
            // block inside Game, so "Game is 97% of the frame" is what a capped frame
            // always looks like and says nothing about who is slow. A bottleneck claim
            // read off a capped frame is unfalsifiable.
            ue_wrap::engine::ExecuteConsoleCommand(L"r.VSync 0");
            ue_wrap::engine::ExecuteConsoleCommand(L"t.MaxFPS 0");
            UE_LOGW("[perf] `stat unit` enabled + vsync/MaxFPS lifted (r.VSync 0, t.MaxFPS 0) -- "
                    "read Frame/Game/Draw/GPU from a screenshot. The uncap is what makes those "
                    "numbers a workload measurement rather than a reading of the cap; our own "
                    "buckets can never attribute engine-side cost.");
        }
    }

    const unsigned long long pe     = GT::PeDispatchCountTotal();
    const unsigned long long peGT   = GT::PeDispatchCountGTTotal();
    const unsigned long long selfNs = GT::PeSelfNsTotal();
    const unsigned long long selfSm = GT::PeSelfSampleTotal();
    const unsigned long long obsNs  = GT::PeObserverBodyNsTotal();
    const unsigned long long frames = g_frames.load(std::memory_order_relaxed);

    const unsigned long long dPE    = pe - g_lastPE;
    const unsigned long long dPEGT  = peGT - g_lastPEGT;
    const unsigned long long dSelf  = selfNs - g_lastSelfNs;
    const unsigned long long dSamp  = selfSm - g_lastSelfSamp;
    const unsigned long long dObs   = obsNs - g_lastObsNs;
    const unsigned long long dFr    = frames - g_lastFrames;
    g_lastPE = pe; g_lastPEGT = peGT; g_lastSelfNs = selfNs; g_lastSelfSamp = selfSm;
    g_lastObsNs = obsNs; g_lastFrames = frames;

    const double pePerSec  = dPE / elapsed;
    const double frPerSec  = dFr / elapsed;
    const double pePerFr   = dFr > 0 ? static_cast<double>(dPE) / dFr : 0.0;
    const double peGTPerFr = dFr > 0 ? static_cast<double>(dPEGT) / dFr : 0.0;
    const double avgSelfNs = dSamp > 0 ? static_cast<double>(dSelf) / dSamp : 0.0;
    // detour ms/frame = avg per-dispatch overhead * dispatches/frame
    const double detourMsFr = dFr > 0 ? (avgSelfNs * pePerFr) / 1e6 : 0.0;
    const double obsMsSec   = (dObs / elapsed) / 1e6;
    const double obsMsFr    = dFr > 0 ? (static_cast<double>(dObs) / dFr) / 1e6 : 0.0;

    UE_LOGW("[perf] PE=%.0f/s (GT=%.0f) frames=%.0f/s => PE/frame=%.0f (GT=%.0f) | obs post=%d pre=%d intc=%d",
            pePerSec, dPEGT / elapsed, frPerSec, pePerFr, peGTPerFr,
            GT::PostObserverCount(), GT::PreObserverCount(), GT::InterceptorCount());

    if (g_selfTime) {
        UE_LOGW("[perf] detour self avg=%.0f ns/dispatch (%llu samp/s) => ~%.2f ms/frame (~%.1f ms/s)",
                avgSelfNs, dSamp, detourMsFr, (avgSelfNs * pePerSec) / 1e6);
        // WHOLE-detour readout. `self` above excludes the outer frame and the SEH
        // __try frame by construction, so it cannot answer "is the detour the
        // unaccounted per-frame cost?" -- it is blind to that region. This one
        // brackets the OUTER detour and subtracts the engine's own ProcessEvent
        // measured on the SAME dispatches, so nothing we add is excluded.
        // WHOLE-self is the size of the blind spot; if it is ~0 the detour is
        // fully accounted for and the missing time is somewhere else entirely.
        const unsigned long long wholeNsTot = GT::PeWholeNsTotal();
        const unsigned long long engNsTot   = GT::PeEngineNsTotal();
        const unsigned long long wSampTot   = GT::PeWholeSampleTotal();
        const unsigned long long topTot     = GT::PeTopLevelCountTotal();
        const unsigned long long dWhole  = wholeNsTot - g_lastWholeNs;
        const unsigned long long dEngine = engNsTot   - g_lastEngineNs;
        const unsigned long long dWSamp  = wSampTot   - g_lastWholeSamp;
        const unsigned long long dTop    = topTot     - g_lastTopLevel;
        g_lastWholeNs = wholeNsTot; g_lastEngineNs = engNsTot;
        g_lastWholeSamp = wSampTot; g_lastTopLevel = topTot;
        if (dWSamp > 0 && dTop > 0) {
            // Per TOP-LEVEL dispatch. `engine` here is a whole nested BP call tree, not
            // one UFunction body, so it is tens of microseconds and is NOT comparable to
            // the per-dispatch `self` figure above -- it exists only to be subtracted.
            const double wholeNs  = static_cast<double>(dWhole)  / dWSamp;
            const double engineNs = static_cast<double>(dEngine) / dWSamp;
            const double oursNs   = wholeNs - engineNs;
            // Scale by the TOP-LEVEL rate, never by pePerSec: the samples are drawn from
            // top-level dispatches only, and every nested dispatch's cost is already
            // inside the bracket.
            const double topPerSec = dTop / elapsed;
            const double oursMsSec = (oursNs * topPerSec) / 1e6;
            UE_LOGW("[perf] detour WHOLE top-level=%.0f/s (%.1f%% of PE) | per top-level: whole=%.0f ns "
                    "engine=%.0f ns OURS=%.0f ns (%llu samp) => OUR TOTAL ~%.2f ms/frame (~%.1f ms/s)",
                    topPerSec, pePerSec > 0 ? 100.0 * topPerSec / pePerSec : 0.0,
                    wholeNs, engineNs, oursNs, dWSamp,
                    frPerSec > 0 ? oursMsSec / frPerSec : 0.0, oursMsSec);
        }
    }

    // Observer/interceptor cb-body total + the single worst body seen (cumulative).
    std::wstring worstName = L"-";
    if (void* wf = GT::PeObserverWorstFn()) worstName = R::ToString(R::NameOf(wf));
    UE_LOGW("[perf] obs/intc body total=%.2f ms/frame (~%.1f ms/s) | worst body '%ls' %.3f ms (cumulative)",
            obsMsFr, obsMsSec, worstName.c_str(), GT::PeObserverWorstNs() / 1e6);

    // Per-subsystem net_pump buckets. One line; ms/frame leads (the budget metric),
    // ms/s in parens (robust when frames aren't being counted, e.g. background window).
    std::wstring line;
    wchar_t cell[96];
    for (size_t i = 0; i < g_buckets.size(); ++i) {
        const unsigned long long cur = g_buckets[i].load(std::memory_order_relaxed);
        const unsigned long long d = cur - g_lastBuckets[i];
        g_lastBuckets[i] = cur;
        const double ms = TicksToMs(d);
        const double msFr = dFr > 0 ? ms / dFr : 0.0;
        const double msSec = ms / elapsed;
        // Only print buckets that cost something so the line stays readable.
        // NetPumpTick + OverlayPresent always print: they are the two per-frame
        // passives the R-3 attribution needs even when near-zero.
        if (msSec < 0.05 && i != static_cast<size_t>(Bucket::NetPumpTick) &&
            i != static_cast<size_t>(Bucket::OverlayPresent)) continue;
        std::swprintf(cell, sizeof(cell) / sizeof(cell[0]), L" %hs=%.2f/fr(%.1f/s)",
                      kBucketNames[i], msFr, msSec);
        line += cell;
    }
    UE_LOGW("[perf] subsystems ms:%ls", line.c_str());
}

}  // namespace coop::dev::perf_probe
