// coop/dev/gnatives_probe.cpp -- see coop/dev/gnatives_probe.h.
//
// Gate 2.2 of docs/COOP_VM_DISPATCH_PLAN.md: measure the frequency + per-dispatch
// cost of EX_LocalVirtualFunction (0x45) / EX_LocalFinalFunction (0x46) so we can
// decide whether the option-A GNatives swap stays under 0.1 ms/frame before any
// consumer code is written.

#include "coop/dev/gnatives_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <thread>

namespace coop::dev::gnatives_probe {
namespace {

namespace GT = ue_wrap::game_thread;

// GNatives handler ABI: void/uintptr exec(UObject* Context, FFrame& Stack, void* Result).
// The dispatcher ignores the return, but execLocalFinal/Virtual DO return a value --
// preserve it so the probe is behaviour-transparent.
using ExecFn = std::uintptr_t(__fastcall*)(void* ctx, void* stack, void* result);

// FFrame +0x20 = Code (bytecode cursor). Measured 2026-07-13.
constexpr std::size_t kFFrameCodeOff = 0x20;

std::uintptr_t* g_gnatives = nullptr;  // GNatives[256], the exec-handler table base.
ExecFn g_origVirtual = nullptr;        // GNatives[0x45] original.
ExecFn g_origFinal   = nullptr;        // GNatives[0x46] original.

// Counters (relaxed atomics -- the dumper reads deltas once a second).
std::atomic<std::uint64_t> g_countGT{0};
std::atomic<std::uint64_t> g_countWorker{0};
std::atomic<std::uint64_t> g_sampleCycles{0};
std::atomic<std::uint64_t> g_sampleCount{0};
std::atomic<std::uint64_t> g_seq{0};  // dispatch sequence -> 1/1024 RDTSC sampling.

// Model the substrate's real filter cost: a fixed 16-slot watch table. Left ALL
// NULL so every dispatch runs the full 16-slot linear scan and MISSES = the
// worst-case (deepest) filter path = a true cost upper bound.
void* g_watch[16] = {nullptr};

double g_tscGHz = 0.0;  // calibrated once at Init.

inline void* PeekCode(void* stack) {
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(stack) + kFFrameCodeOff);
}

// The added work of the real wrapper: peek the callee-identity operand + the
// two-stage filter's first stage (16-slot identity scan). Returns whether matched
// (always false here -- empty watch table), kept so the compiler can't elide it.
template <int OP>
inline bool ModelFilter(void* stack) {
    void* code = PeekCode(stack);
    std::uintptr_t key;
    if (OP == 0x46) {
        // EX_LocalFinalFunction: 8-byte serialized UFunction* operand.
        key = *reinterpret_cast<std::uintptr_t*>(code);
    } else {
        // EX_LocalVirtualFunction: 12-byte FScriptName {int32, int32, int32}.
        const std::uint32_t* p = reinterpret_cast<std::uint32_t*>(code);
        key = static_cast<std::uintptr_t>(p[0]) ^ (static_cast<std::uintptr_t>(p[2]) << 32);
    }
    for (int i = 0; i < 16; ++i) {
        if (g_watch[i] == reinterpret_cast<void*>(key)) return true;
    }
    return false;
}

template <int OP>
std::uintptr_t __fastcall Wrapper(void* ctx, void* stack, void* result) {
    const bool sample = (g_seq.fetch_add(1, std::memory_order_relaxed) & 1023) == 0;
    const std::uint64_t t0 = sample ? __rdtsc() : 0;

    // ---- the substrate's added work (cost upper bound) ----
    volatile bool matched = ModelFilter<OP>(stack);
    (void)matched;
    if (GT::IsGameThread())
        g_countGT.fetch_add(1, std::memory_order_relaxed);
    else
        g_countWorker.fetch_add(1, std::memory_order_relaxed);

    if (sample) {
        g_sampleCycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        g_sampleCount.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- tail-call the untouched original handler (behaviour-transparent) ----
    return (OP == 0x46 ? g_origFinal : g_origVirtual)(ctx, stack, result);
}

// Resolve GNatives from a dispatch site: `4C 8D 0D <rel32>` (lea r9, GNatives) that
// is immediately followed by the r15-based dispatch tail. rel32 is RIP-relative to
// the end of the LEA (match + 7).
std::uintptr_t* ResolveGNatives() {
    const uintptr_t hit = ue_wrap::FindPattern(
        "4C 8D 0D ?? ?? ?? ?? 49 8B D7 0F B6 08 48 FF C0 49 89 47 20 8B C1 49 8B 4F 18 41 FF 14 C1");
    if (!hit) return nullptr;
    const std::int32_t rel = *reinterpret_cast<std::int32_t*>(hit + 3);
    return reinterpret_cast<std::uintptr_t*>(hit + 7 + rel);
}

// Sanity-check the resolved table: most of the 256 slots must be code pointers
// inside the main module's image (the exec handlers). Guards against a bad AOB
// match resolving to the wrong global.
bool ValidateTable(const std::uintptr_t* tbl) {
    uintptr_t base = 0, size = 0;
    ue_wrap::MainModuleRange(base, size);
    int inRange = 0;
    for (int i = 0; i < 256; ++i) {
        const std::uintptr_t p = tbl[i];
        if (p >= base && p < base + size) ++inRange;
    }
    return inRange >= 200;  // ~all 256 point at exec handlers in .text.
}

void CalibrateTsc() {
    LARGE_INTEGER qf{}, q0{}, q1{};
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    const std::uint64_t c0 = __rdtsc();
    ::Sleep(50);
    QueryPerformanceCounter(&q1);
    const std::uint64_t c1 = __rdtsc();
    const double secs = double(q1.QuadPart - q0.QuadPart) / double(qf.QuadPart);
    g_tscGHz = (secs > 0.0) ? (double(c1 - c0) / secs) / 1e9 : 0.0;
}

void DumperThread() {
    std::uint64_t lastGT = 0, lastWk = 0, lastCyc = 0, lastN = 0;
    for (;;) {
        ::Sleep(1000);
        const std::uint64_t gt = g_countGT.load(std::memory_order_relaxed);
        const std::uint64_t wk = g_countWorker.load(std::memory_order_relaxed);
        const std::uint64_t cyc = g_sampleCycles.load(std::memory_order_relaxed);
        const std::uint64_t n = g_sampleCount.load(std::memory_order_relaxed);

        const std::uint64_t dGT = gt - lastGT;
        const std::uint64_t dWk = wk - lastWk;
        const std::uint64_t dCyc = cyc - lastCyc;
        const std::uint64_t dN = n - lastN;
        lastGT = gt; lastWk = wk; lastCyc = cyc; lastN = n;

        const double avgCyc = dN ? (double(dCyc) / double(dN)) : 0.0;
        const double nsPerCall = (g_tscGHz > 0.0) ? (avgCyc / g_tscGHz) : 0.0;
        // GT is the only thread that costs frame time. added ms this second on GT:
        const double gtMsPerSec = (double(dGT) * nsPerCall) / 1e6;

        UE_LOGI("[gnatives_probe] 1s: GT=%llu/s worker=%llu/s | avg=%.1f cyc/call "
                "(%.1f ns, %llu samp) | GT added=%.3f ms/s => %.4f ms/frame@120 "
                "%.4f ms/frame@60 (gate <=0.1)",
                (unsigned long long)dGT, (unsigned long long)dWk, avgCyc, nsPerCall,
                (unsigned long long)dN, gtMsPerSec, gtMsPerSec / 120.0, gtMsPerSec / 60.0);
    }
}

}  // namespace

void Init() {
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    if (!coop::config::IsIniKeyTrue("gnatives_probe")) return;

    g_gnatives = ResolveGNatives();
    if (!g_gnatives) {
        UE_LOGE("[gnatives_probe] GNatives AOB unresolved -- probe DISABLED");
        return;
    }
    if (!ValidateTable(g_gnatives)) {
        UE_LOGE("[gnatives_probe] GNatives@%p failed validation -- probe DISABLED", (void*)g_gnatives);
        return;
    }

    CalibrateTsc();

    DWORD oldProt = 0;
    if (!VirtualProtect(&g_gnatives[0x45], sizeof(void*) * 2, PAGE_READWRITE, &oldProt)) {
        UE_LOGE("[gnatives_probe] VirtualProtect failed -- probe DISABLED");
        return;
    }
    g_origVirtual = reinterpret_cast<ExecFn>(g_gnatives[0x45]);
    g_origFinal   = reinterpret_cast<ExecFn>(g_gnatives[0x46]);
    g_gnatives[0x45] = reinterpret_cast<std::uintptr_t>(&Wrapper<0x45>);
    g_gnatives[0x46] = reinterpret_cast<std::uintptr_t>(&Wrapper<0x46>);
    VirtualProtect(&g_gnatives[0x45], sizeof(void*) * 2, oldProt, &oldProt);

    UE_LOGI("[gnatives_probe] ARMED: GNatives@%p [0x45]->%p [0x46]->%p (orig V=%p F=%p) "
            "tsc=%.2f GHz -- dumping EX_Local* rate 1/s",
            (void*)g_gnatives, (void*)&Wrapper<0x45>, (void*)&Wrapper<0x46>,
            (void*)g_origVirtual, (void*)g_origFinal, g_tscGHz);

    std::thread(DumperThread).detach();
}

}  // namespace coop::dev::gnatives_probe
