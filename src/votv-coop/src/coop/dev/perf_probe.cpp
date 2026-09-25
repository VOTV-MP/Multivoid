// coop/dev/perf_probe.cpp -- see coop/dev/perf_probe.h. A measure-first probe: it owns the
// frame counter and the per-subsystem tick buckets, and reads the detour's dispatch,
// self-time and observer-body counters out of the game-thread layer, which owns them
// because the detour lives there. Once a second it logs the rates and per-frame costs.

#include "coop/dev/perf_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/walk_census.h"

#include <windows.h>
#include <psapi.h>  // PROCESS_MEMORY_COUNTERS_EX + K32GetProcessMemoryInfo (kernel32 export; no psapi.lib link)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace coop::dev::perf_probe {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R = ue_wrap::reflection;

// Atomic because the readers are on two threads: Scope/AddTicks run on the game thread and
// from the overlay's present path on the render thread.
std::atomic<bool> g_armed{false};  // set by Init() from the ini; read by Armed()
bool  g_selfTime  = false;
bool  g_initDone  = false;
int   g_resDropAfterS = 0;   // perf_probe_resdrop: samples before the render-res collapse
int   g_bypassAfterS  = 0;   // perf_probe_bypass: samples before the transparent-bypass window
int   g_statUnitAfterS = 0;  // perf_probe_statunit: samples before `stat unit` (0 = never issue it)
bool  g_dispatch    = false; // perf_probe_dispatch: arm the per-dispatch counters (not free)

// The frame counter (incremented from the present detour) and the subsystem buckets.
std::atomic<unsigned long long> g_frames{0};
std::array<std::atomic<unsigned long long>, static_cast<size_t>(Bucket::Count)> g_buckets{};

const char* kBucketNames[static_cast<size_t>(Bucket::Count)] = {
    "netPumpTick", "reaper", "localSend", "installObs", "interactable", "weatherConn",
    "itemConn", "trashWatch", "balance", "snapshotDrain", "remoteProp", "puppets",
    "eventFeed", "inputOwner", "nameplate", "roster", "overlayPresent",
};

long long QpcFreq() {
    static long long s_freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    return s_freq;
}

// The 1 Hz sampler window state, game thread only (Sample runs there).
std::chrono::steady_clock::time_point g_lastSample{};
bool g_haveBaseline = false;
unsigned long long g_lastPECoop = 0;
// The reflected-call window: the call total and the ParamFrame counters, which are counted
// whether or not the dispatch knobs are armed.
unsigned long long g_lastCalls = 0, g_lastPfFrames = 0, g_lastPfAllocs = 0, g_lastPfBytes = 0;
unsigned long long g_lastWalks = 0;  // the finders' whole-array walks (walk_census)
unsigned long long g_lastPE = 0, g_lastPEGT = 0, g_lastSelfNs = 0, g_lastSelfSamp = 0,
                   g_lastObsNs = 0, g_lastFrames = 0;
// The whole-detour window state; see the whole readout in Sample.
unsigned long long g_lastWholeNs = 0, g_lastEngineNs = 0, g_lastWholeSamp = 0, g_lastTopLevel = 0;
std::array<unsigned long long, static_cast<size_t>(Bucket::Count)> g_lastBuckets{};

// Every tenth summary: which call sites the window's whole-array walks came from, as module+RVA (the
// payload's .map resolves them), most first. A finder called by another finder names that finder. The
// first summary sets the baseline, so each line covers the ten summaries before it; walks the site
// table could not hold are named as such.
void LogWalkSitesEveryTenth() {
    namespace WC = ue_wrap::walk_census;
    struct Row { void* ip; unsigned long long d; };
    static int s_n = 0;
    static unsigned long long s_last[WC::kSiteSlots] = {};
    static unsigned long long s_lastUnattributed = 0;
    static Row rows[WC::kSiteSlots];
    const bool baseline = s_n == 0;
    if (s_n++ % 10 != 0) return;
    int nRows = 0;
    for (int i = 0; i < WC::kSiteSlots; ++i) {
        void* ip = nullptr;
        unsigned long long c = 0;
        if (!WC::ArrayWalkSiteAt(i, &ip, &c)) break;
        const unsigned long long d = c - s_last[i];
        s_last[i] = c;
        if (ip && d) rows[nRows++] = {ip, d};
    }
    const unsigned long long unattributed = WC::ArrayWalkUnattributedTotal();
    const unsigned long long dUnattributed = unattributed - s_lastUnattributed;
    s_lastUnattributed = unattributed;
    if (baseline || (nRows == 0 && dUnattributed == 0)) return;
    const int shown = nRows < 6 ? nRows : 6;
    std::partial_sort(rows, rows + shown, rows + nRows, [](const Row& a, const Row& b) { return a.d > b.d; });
    std::string out;
    for (int i = 0; i < shown; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = {};
        uintptr_t rva = 0;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(rows[i].ip), &mod) && mod) {
            ::GetModuleFileNameA(mod, name, MAX_PATH);
            rva = reinterpret_cast<uintptr_t>(rows[i].ip) - reinterpret_cast<uintptr_t>(mod);
        }
        const char* base = std::strrchr(name, '\\');
        char cell[160];
        std::snprintf(cell, sizeof(cell), "%s%s+0x%llX x%llu", out.empty() ? "" : ", ", base ? base + 1 : "?",
                      static_cast<unsigned long long>(rva), rows[i].d);
        out += cell;
    }
    if (dUnattributed) {
        char cell[96];
        std::snprintf(cell, sizeof(cell), "%sunattributed (the site table is full) x%llu", out.empty() ? "" : ", ",
                      dUnattributed);
        out += cell;
    }
    UE_LOGW("[perf] finder walks by call site (10 s): %s", out.c_str());
}

}  // namespace

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::perf_probe);
    return s;
}

bool Armed() { return g_armed.load(std::memory_order_relaxed); }

double TicksToMs(unsigned long long ticks) {
    const long long f = QpcFreq();
    return f > 0 ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(f) : 0.0;
}

unsigned long long NowTicks() {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return static_cast<unsigned long long>(t.QuadPart);
}

void AddTicks(Bucket b, unsigned long long ticks) {
    if (!Armed()) return;
    const size_t i = static_cast<size_t>(b);
    if (i < g_buckets.size()) g_buckets[i].fetch_add(ticks, std::memory_order_relaxed);
}

Scope::Scope(Bucket b) : b_(b), on_(Armed()), t0_(0) {
    if (on_) t0_ = NowTicks();
}
Scope::~Scope() {
    if (on_) AddTicks(b_, NowTicks() - t0_);
}

void Init() {
    if (g_initDone) return;
    g_initDone = true;
    if (!Enabled()) return;
    g_armed.store(true, std::memory_order_relaxed);
    g_selfTime = coop::config::ResolveFlag(::coop::config_registry::rows::perf_probe_selftime);
    g_resDropAfterS = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::perf_probe_resdrop));
    g_bypassAfterS  = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::perf_probe_bypass));
    g_statUnitAfterS = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::perf_probe_statunit));
    // Per-dispatch counting is the probe's own hot path and is opt-in. The frame counter and
    // the subsystem buckets ride per-frame and per-subsystem-call edges (single digits per
    // second); the dispatch counters ride hundreds of thousands of dispatches per second across
    // threads and share one cache line, so leaving them on would make the cheapest baseline,
    // the mod merely watching, cost something unmeasured.
    g_dispatch = coop::config::ResolveFlag(::coop::config_registry::rows::perf_probe_dispatch);
    g_selfTime = g_selfTime && g_dispatch;
    GT::SetPerfCounting(g_dispatch, g_selfTime);
    ue_wrap::script_gate::SetPerfCounting(g_dispatch);  // the script-loop call count, the gate's own tax base
    R::SetCoopCallCensus(g_dispatch);   // attribute the blueprint calls our polls author
    UE_LOGW("[perf] probe ARMED (perf_probe=1, dispatch=%d, selftime=%d, statunit=%d) -- 1 Hz "
            "frame-cost report follows. At dispatch=0/statunit=0 this is a frame counter plus "
            "subsystem buckets and costs ~nothing; each of those two knobs makes the probe "
            "change the frame it is measuring, so turn them off for any number that is meant "
            "to say what the MOD costs.",
            g_dispatch ? 1 : 0, g_selfTime ? 1 : 0, g_statUnitAfterS);
}

void NoteFrame() {
    // Both Init and Sample are driven from the net pump's tick, which does not run outside a
    // coop session, so the probe would produce no data for a loaded mod that is not hosting,
    // exactly the baseline that splits the DLL's resident cost from the session's. Init is
    // posted too, not just Sample: Init is the only thing that arms the probe, so gating this on
    // the armed flag would keep the probe from arming without a session. Init self-latches and
    // Sample self-throttles to 1 Hz, so this is at most one posted task per second and a no-op
    // whenever the pump already sampled.
    static ULONGLONG sNextPost = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now >= sNextPost) {
        sNextPost = now + 1000;
        ue_wrap::game_thread::Post([] { Init(); Sample(); });
    }
    if (!Armed()) return;
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

void Sample() {
    if (!Armed()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!g_haveBaseline) {
        g_haveBaseline = true;
        g_lastSample = now;
        // The reflected-call counters are cumulative from process start, so a window that begins
        // at zero reports the whole boot as one second's rate. Seed them with the clock.
        g_lastCalls = R::CoopCallCountTotal();
        g_lastWalks = ue_wrap::walk_census::ArrayWalkCountTotal();
        const ue_wrap::FrameStats fs0 = ue_wrap::GetFrameStats();
        g_lastPfFrames = fs0.frames; g_lastPfAllocs = fs0.allocs; g_lastPfBytes = fs0.bytes;
        return;  // establish the first baseline; report from the next window
    }
    const double elapsed = std::chrono::duration<double>(now - g_lastSample).count();
    if (elapsed < 1.0) return;
    g_lastSample = now;

    // The memory line: a client that balloons slowly to an out-of-memory kill leaves no log
    // signature, so this 1 Hz line shows the climb and its rate. Private (commit) bytes are the
    // indicator, since they grow with leaked heap regardless of working-set trimming; the
    // process memory query is a kernel32 export, no extra link.
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            UE_LOGW("[perf][mem] private(commit)=%.1f MB workingset=%.1f MB -- watch for steady climb (RAM-balloon hunt)",
                    pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0);
        }
    }

    // The engine's unit stat. Our own buckets can only account for our code; they cannot say
    // whether a lost millisecond went to the game thread, the render thread or the GPU, and the
    // engine's own unit graph is the only thing that splits the frame three ways. Deferred to
    // Sample rather than Init because it needs a world; retried until the call succeeds. Opt-in
    // and delayed, because it is an instrument that mutates what it measures: enabling any
    // engine stat arms stats collection across the whole engine, and the vsync and frame-cap
    // commands rewrite the player's own frame pacing. The samples before this line are the
    // uninstrumented baseline on the same world at the same spot, so the step across it is this
    // instrument's own cost, reported rather than assumed.
    if (g_statUnitAfterS > 0) {
        static int sSU = 0;
        static bool sStatUnitOn = false;
        if (!sStatUnitOn && ++sSU >= g_statUnitAfterS
                && ue_wrap::engine::ExecuteConsoleCommand(L"stat unit")) {
            sStatUnitOn = true;
            // Uncap first, or every number is a reading of the cap and not of the workload: under
            // vsync the engine's game thread blocks on the frame sync and the unit stat counts that
            // block inside Game, so a Game that is most of the frame is what a capped frame always
            // looks like and says nothing about who is slow; a bottleneck claim read off a capped
            // frame is unfalsifiable.
            ue_wrap::engine::ExecuteConsoleCommand(L"r.VSync 0");
            ue_wrap::engine::ExecuteConsoleCommand(L"t.MaxFPS 0");
            UE_LOGW("[perf] STAT-UNIT ARMED after %d samples -- `stat unit` + r.VSync 0 + "
                    "t.MaxFPS 0 issued. READ THE fps EITHER SIDE OF THIS LINE: the samples "
                    "above are the uninstrumented baseline on this same world, the ones below "
                    "carry FThreadStats, so the step down across this line is what this "
                    "instrument itself costs. Then read Frame/Game/Draw/GPU from a screenshot. "
                    "NOTE the uncap is a REQUEST, not a fact -- verify fps actually exceeds the "
                    "old ceiling before calling any number below a workload measurement.", sSU);
        }
    }

    // The CPU-versus-GPU discriminator (perf_probe_resdrop). The unit stat alone cannot settle
    // which side is the bottleneck: when the GPU is the limiter the game thread blocks waiting
    // on it, so Game inflates to the frame and looks like the culprit, the same shape a genuinely
    // CPU-bound frame has. Collapsing the render resolution changes only the GPU's workload, so
    // it separates them by construction: the frame time falls a lot when GPU-bound and barely
    // moves when CPU-bound. Fires some seconds in, so there is a full-resolution baseline in the
    // same run, on the same world, at the same spot.
    if (g_resDropAfterS > 0) {
        static int sSamples = 0;
        static bool sDropped = false;
        if (!sDropped && ++sSamples >= g_resDropAfterS) {
            sDropped = true;
            ue_wrap::engine::ExecuteConsoleCommand(L"r.ScreenPercentage 25");
            UE_LOGW("[perf] RES-DROP: r.ScreenPercentage 25 issued after %d samples -- compare "
                    "Frame/GPU either side of this line. A big drop means GPU-bound; little "
                    "change means CPU-bound.", sSamples);
        }
    }

    // Which of our reflected calls make up the blueprint dispatches per frame we author.
    // Cumulative shares, every 10 s (resolving names walks reflection, so it is deliberately
    // rare). This is the list to cut from: each of these runs real game blueprint on the game
    // thread.
    {
        static int sCallTick = 0;
        if (++sCallTick % 10 == 0) {
            struct Row { void* fn; unsigned long long n; };
            Row top[8] = {};
            unsigned long long total = 0;
            for (int i = 0; ; ++i) {
                void* fn = nullptr; unsigned long long n = 0;
                if (!R::CoopCallSiteAt(i, &fn, &n)) break;
                total += n;
                for (int k = 0; k < 8; ++k) {
                    if (n > top[k].n) {
                        for (int j = 7; j > k; --j) top[j] = top[j - 1];
                        top[k] = {fn, n};
                        break;
                    }
                }
            }
            if (total > 0) {
                std::wstring line;
                for (int k = 0; k < 8 && top[k].fn; ++k) {
                    wchar_t buf[200];
                    std::swprintf(buf, 200, L"%ls=%.1f%%(%llu) ",
                                  R::ToString(R::NameOf(top[k].fn)).c_str(),
                                  100.0 * top[k].n / total, top[k].n);
                    line += buf;
                }
                UE_LOGW("[perf] OUR CALLS (cumulative %llu): %ls", total, line.c_str());
            }
        }
    }
    // The discriminator for what is left. Our measured code is a small fraction of the frame and
    // the frame rate does not correlate with it, so the missing time is engine work our presence
    // provokes. The transparent bypass forwards ProcessEvent straight through, no observers,
    // interceptors or posted-task pump, while our threads and every actor we spawned stay as they
    // are, so it splits the remaining space in half: the frame rate recovers if the cost is in
    // the dispatch path, and stays unchanged if the cost is what we put in the world. Sample
    // rides a posted task, so it cannot run during the window; the first sample after it covers
    // the window, and its frames over elapsed is the bypassed rate.
    if (g_bypassAfterS > 0) {
        static int sB = 0;
        static bool sArmed = false;
        if (!sArmed && ++sB >= g_bypassAfterS) {
            sArmed = true;
            UE_LOGW("[perf] BYPASS ARMED for 5000 ms -- the NEXT [perf] line's frames/s is the "
                    "bypassed rate (our dispatch path inert, our actors and threads untouched). "
                    "Compare it against the lines above.");
            GT::SetTransparentBypass(5000);
        }
    }
    const unsigned long long pe     = GT::PeDispatchCountTotal();
    const unsigned long long peGT   = GT::PeDispatchCountGTTotal();
    const unsigned long long peCoop = GT::PeDispatchCountCoopTotal();
    const unsigned long long selfNs = GT::PeSelfNsTotal();
    const unsigned long long selfSm = GT::PeSelfSampleTotal();
    const unsigned long long obsNs  = GT::PeObserverBodyNsTotal();
    const unsigned long long frames = g_frames.load(std::memory_order_relaxed);

    const unsigned long long dPE    = pe - g_lastPE;
    const unsigned long long dPEGT  = peGT - g_lastPEGT;
    const unsigned long long dCoop  = peCoop - g_lastPECoop;
    const unsigned long long dSelf  = selfNs - g_lastSelfNs;
    const unsigned long long dSamp  = selfSm - g_lastSelfSamp;
    const unsigned long long dObs   = obsNs - g_lastObsNs;
    const unsigned long long dFr    = frames - g_lastFrames;
    g_lastPE = pe; g_lastPEGT = peGT; g_lastSelfNs = selfNs; g_lastSelfSamp = selfSm;
    g_lastPECoop = peCoop;
    g_lastObsNs = obsNs; g_lastFrames = frames;

    const double pePerSec  = dPE / elapsed;
    const double frPerSec  = dFr / elapsed;
    const double pePerFr   = dFr > 0 ? static_cast<double>(dPE) / dFr : 0.0;
    const double peGTPerFr = dFr > 0 ? static_cast<double>(dPEGT) / dFr : 0.0;
    const double avgSelfNs = dSamp > 0 ? static_cast<double>(dSelf) / dSamp : 0.0;
    // Detour ms per frame: the average per-dispatch overhead times the dispatches per frame.
    const double detourMsFr = dFr > 0 ? (avgSelfNs * pePerFr) / 1e6 : 0.0;
    const double obsMsSec   = (dObs / elapsed) / 1e6;
    const double obsMsFr    = dFr > 0 ? (static_cast<double>(dObs) / dFr) / 1e6 : 0.0;

    // The frame rate rides its own line, so it is greppable in both probe modes and can never be
    // read off a line whose other fields are zeroed with dispatch counting off.
    UE_LOGW("[perf] fps=%.0f (frame=%.2f ms) | obs post=%d pre=%d intc=%d",
            frPerSec, frPerSec > 0 ? 1000.0 / frPerSec : 0.0,
            GT::PostObserverCount(), GT::PreObserverCount(), GT::InterceptorCount());

    // The reflected-call rate: what OUR code dispatches through reflection, and how much of that
    // builds a parameter frame on the heap. Ungated, because the counters behind it are -- a rate
    // that needs a knob armed is a rate no ordinary session ever records. Rates come from the
    // window; the size distribution is cumulative, since it answers what an inline buffer would
    // cover and not how often one was used.
    {
        const unsigned long long calls = R::CoopCallCountTotal();
        const unsigned long long walks = ue_wrap::walk_census::ArrayWalkCountTotal();
        const unsigned long long dWalks = walks - g_lastWalks;
        g_lastWalks = walks;
        const ue_wrap::FrameStats fs = ue_wrap::GetFrameStats();
        const unsigned long long dCalls = calls - g_lastCalls;
        const unsigned long long dPf    = fs.frames - g_lastPfFrames;
        const unsigned long long dAlloc = fs.allocs - g_lastPfAllocs;
        const unsigned long long dBytes = fs.bytes  - g_lastPfBytes;
        g_lastCalls = calls; g_lastPfFrames = fs.frames;
        g_lastPfAllocs = fs.allocs; g_lastPfBytes = fs.bytes;
        const double allocPerSec = dAlloc / elapsed;
        // The size cells are DISJOINT bands, not nested thresholds: a frame lands in exactly one,
        // and the last cell is the residue above the largest band, printed so the reader never has
        // to subtract.
        unsigned long long banded = 0;
        for (int i = 0; i < 5; ++i) banded += fs.bucket[i];
        UE_LOGW("[perf] reflected calls=%.0f/s (%.1f/frame) | finder walks=%.1f/s | ParamFrame=%.0f/s "
                "alloc=%.0f/s (%.1f KB/s) | sizes 0-16:%llu 17-32:%llu 33-64:%llu 65-128:%llu 129-256:%llu "
                "over-256:%llu max=%d (cumulative %llu allocs of %llu frames)",
                dCalls / elapsed, dFr > 0 ? static_cast<double>(dCalls) / dFr : 0.0, dWalks / elapsed,
                dPf / elapsed, allocPerSec, (dBytes / elapsed) / 1024.0,
                fs.bucket[0], fs.bucket[1], fs.bucket[2], fs.bucket[3], fs.bucket[4],
                fs.allocs - banded, fs.maxSize, fs.allocs, fs.frames);
        LogWalkSitesEveryTenth();
    }

    if (g_dispatch) {
        UE_LOGW("[perf] PE=%.0f/s (GT=%.0f, OURS=%.0f = %.1f%% of GT) frames=%.0f/s => PE/frame=%.0f (GT=%.0f)",
                pePerSec, dPEGT / elapsed, dCoop / elapsed,
                dPEGT > 0 ? 100.0 * dCoop / dPEGT : 0.0, frPerSec, pePerFr, peGTPerFr);
        // The script-body gate's tax base: every script body the VM ran, the game-thread share,
        // and how many reached a watch; the per-call cost is one load and a branch while the
        // session gate is closed, one hashed probe per key space while it is open.
        static unsigned long long sLastGateCalls = 0, sLastGateGT = 0, sLastGateMatched = 0;
        const auto gs = ue_wrap::script_gate::GetStats();
        const unsigned long long dGate = gs.calls - sLastGateCalls;
        const unsigned long long dGateGT = gs.callsGameThread - sLastGateGT;
        const unsigned long long dGateHit = gs.matched - sLastGateMatched;
        sLastGateCalls = gs.calls; sLastGateGT = gs.callsGameThread; sLastGateMatched = gs.matched;
        UE_LOGW("[perf] script-loop=%.0f/s (GT=%.0f) watched=%.0f/s cancelled=%llu total, watches=%d+%d, "
                "off-thread hits=%llu, gate %s",
                dGate / elapsed, dGateGT / elapsed, dGateHit / elapsed, gs.cancelled, gs.watches,
                gs.nameWatches, gs.offGameThread, gs.enabled ? "open" : "closed");
    }

    if (g_selfTime) {
        UE_LOGW("[perf] detour self avg=%.0f ns/dispatch (%llu samp/s) => ~%.2f ms/frame (~%.1f ms/s)",
                avgSelfNs, dSamp, detourMsFr, (avgSelfNs * pePerSec) / 1e6);
        // The whole-detour readout. The self figure above excludes the outer frame and the SEH
        // frame by construction, so it cannot say whether the detour is the unaccounted per-frame
        // cost; this one brackets the outer detour and subtracts the engine's own ProcessEvent
        // measured on the same dispatches, so nothing we add is excluded. The difference is the
        // size of the blind spot; near zero, the detour is fully accounted for and the missing time
        // is elsewhere.
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
            // Per top-level dispatch. The engine figure here is a whole nested blueprint call tree,
            // not one UFunction body, so it is tens of microseconds and not comparable to the
            // per-dispatch self figure above; it exists only to be subtracted.
            const double wholeNs  = static_cast<double>(dWhole)  / dWSamp;
            const double engineNs = static_cast<double>(dEngine) / dWSamp;
            const double oursNs   = wholeNs - engineNs;
            // Scale by the top-level rate, never by the dispatch rate: the samples are drawn from
            // top-level dispatches only, and every nested dispatch's cost is already inside the
            // bracket.
            const double topPerSec = dTop / elapsed;
            const double oursMsSec = (oursNs * topPerSec) / 1e6;
            UE_LOGW("[perf] detour WHOLE top-level=%.0f/s (%.1f%% of PE) | per top-level: whole=%.0f ns "
                    "engine=%.0f ns OURS=%.0f ns (%llu samp) => OUR TOTAL ~%.2f ms/frame (~%.1f ms/s)",
                    topPerSec, pePerSec > 0 ? 100.0 * topPerSec / pePerSec : 0.0,
                    wholeNs, engineNs, oursNs, dWSamp,
                    frPerSec > 0 ? oursMsSec / frPerSec : 0.0, oursMsSec);
        }
    }

    // The observer and interceptor body total, and the single worst body seen (cumulative).
    std::wstring worstName = L"-";
    if (void* wf = GT::PeObserverWorstFn()) worstName = R::ToString(R::NameOf(wf));
    UE_LOGW("[perf] obs/intc body total=%.2f ms/frame (~%.1f ms/s) | worst body '%ls' %.3f ms (cumulative)",
            obsMsFr, obsMsSec, worstName.c_str(), GT::PeObserverWorstNs() / 1e6);

    // The per-subsystem pump buckets. One line; ms per frame leads (the budget metric), ms per
    // second in parentheses (robust when frames are not being counted, as in a background
    // window).
    std::wstring line;
    wchar_t cell[96];
    for (size_t i = 0; i < g_buckets.size(); ++i) {
        const unsigned long long cur = g_buckets[i].load(std::memory_order_relaxed);
        const unsigned long long d = cur - g_lastBuckets[i];
        g_lastBuckets[i] = cur;
        const double ms = TicksToMs(d);
        const double msFr = dFr > 0 ? ms / dFr : 0.0;
        const double msSec = ms / elapsed;
        // Only print buckets that cost something, so the line stays readable; the pump tick and the
        // overlay present always print, since they are the two per-frame passives the attribution
        // needs even when near zero.
        if (msSec < 0.05 && i != static_cast<size_t>(Bucket::NetPumpTick) &&
            i != static_cast<size_t>(Bucket::OverlayPresent)) continue;
        std::swprintf(cell, sizeof(cell) / sizeof(cell[0]), L" %hs=%.2f/fr(%.1f/s)",
                      kBucketNames[i], msFr, msSec);
        line += cell;
    }
    UE_LOGW("[perf] subsystems ms:%ls", line.c_str());
}

}  // namespace coop::dev::perf_probe
