// coop/dev/gnatives_probe.cpp -- see coop/dev/gnatives_probe.h.
//
// STEP 1.0 of docs/COOP_VM_DISPATCH_PLAN.md (impl /qf 2026-07-13, rounds 11-15):
// the probe-first REAL-FILTER gate that MUST pass before the permanent, never-un-
// swapped GNatives[0x45] substrate lands. This is NOT the old 16-slot toy scan --
// it runs the SAME name-first filter shape the incr-1 wrapper will ship (factored
// into RunKerfurFilter below so it is liftable into ue_wrap/vm_dispatch verbatim),
// so the <=0.1 ms/frame measurement is of the real prologue, not a stand-in.
//
// It measures, in ONE game run:
//   (i)  LIVE-CATCH + POSITIVE CONTROL -- swaps GNatives[0x45] ONLY (0x46 has no
//        measured customer; plan R11), resolves the two kerfur verb FNames
//        (dropKerfurProp / spawnKerfuro) + classes on the GT once, then per 0x45
//        dispatch does IsGameThread -> 8-byte NAME compare (the POSITIVE CONTROL:
//        a raw name-match proves the flip verb dispatched, independent of the class
//        filter) -> IsDescendantOfAny class CONFIRM. Logs the first N name-matches
//        (operand bytes + Context class + thread) so "0x45 is the flip opener" is a
//        LIVE catch, not a static bytecode inference.
//   (ii) PERF -- enabled path (real filter) vs the disabled fast-path tax
//        (ini gnatives_probe_disabled=1 -> the eternal solo-SP cost of the
//        never-removed swap: 1 relaxed atomic load + branch + tail-call). Sampled
//        RDTSC; the 1/s dumper derives ms/frame.
//
// HARD HALT interpretation of the log (docs/COOP_VM_DISPATCH_PLAN.md S2.0):
//   - name-match > 0 (positive control fires) AND class-confirmed == name-match
//     AND enabled+disabled both <= 0.1 ms/frame => PASS, build the permanent swap.
//   - name-match > 0 but class-confirmed < name-match => a kerfur variant is being
//     REJECTED by the class confirm; its class is in the log -> register it (the
//     class is a confirm, not a gate, so name still caught it -- but investigate).
//   - flip triggered but name-match == 0 => the 8-byte compare / operand layout is
//     wrong (the log dumps the raw operand bytes to diff vs StringToFName) -> HALT.
//   - no flip triggered => unexercised INVALID run, re-run with a forced toggle.
//
// THROWAWAY / diagnostics (RULE 2 exempt); does NOT ship (RULE 3). Retired with
// the substrate work (increment 4).

#include "coop/dev/gnatives_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <string>
#include <thread>

namespace coop::dev::gnatives_probe {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;

// GNatives handler ABI: void/uintptr exec(UObject* Context, FFrame& Stack, void* Result).
// The dispatcher ignores the return, but execLocalVirtual DOES return a value -- preserve it.
using ExecFn = std::uintptr_t(__fastcall*)(void* ctx, void* stack, void* result);

// FFrame +0x20 = Code (bytecode cursor). At wrapper entry for op 0x45 the cursor
// points AT the 12-byte FScriptName operand {ComparisonIndex@0, DisplayIndex@4, Number@8}
// (LIVE-MEASURED v2/v3 2026-07-13 -- CmpIdx==DispIdx in shipping so op[0]==op[1]; Number@8=0
// for the clean verb names; the spike's {CmpIdx,Number@4,Display@8} was wrong on Number's slot.
// Match op[0]==StringToFName.ComparisonIndex && op[8]==Number, NOT raw bytes 0-7).
constexpr std::size_t kFFrameCodeOff = 0x20;

std::uintptr_t* g_gnatives = nullptr;  // GNatives[256], the exec-handler table base.
ExecFn g_origVirtual = nullptr;        // GNatives[0x45] original.
ExecFn g_origFinal   = nullptr;        // GNatives[0x46] original.

bool g_enabled = true;  // false via ini gnatives_probe_disabled=1 -> measure the pure tax.

// ---- the filter's resolved constants (set ONCE on the GT; read relaxed per dispatch) ----
// Packed FName = uint32(ComparisonIndex) | (uint64)(uint32(Number)) << 32. 0 = unresolved.
std::atomic<std::uint64_t> g_verbDropProp{0};   // dropKerfurProp (turn-off verb)
std::atomic<std::uint64_t> g_verbSpawnKerf{0};  // spawnKerfuro    (turn-on verb)
std::atomic<void*> g_npcClass{nullptr};         // kerfurOmega_C
std::atomic<void*> g_propClass{nullptr};        // prop_kerfurOmega_C
std::atomic<bool>  g_resolved{false};

// ---- counters (relaxed atomics -- the dumper reads deltas once a second) ----
std::atomic<std::uint64_t> g_countGT{0};       // 0x45 dispatches on the game thread
std::atomic<std::uint64_t> g_countWorker{0};   // 0x45 dispatches off the game thread
std::atomic<std::uint64_t> g_nameMatch{0};     // POSITIVE CONTROL: raw name==verb (pre-class)
std::atomic<std::uint64_t> g_classConfirmed{0};// name-match AND class descends kerfur family
std::atomic<std::uint64_t> g_offGtMatch{0};    // name-match seen OFF the GT (tripwire)
std::atomic<std::uint64_t> g_kerfurCtx45{0};   // v2 DIAG: ANY 0x45 dispatch with a kerfur Context
std::atomic<std::uint64_t> g_kerfurCtx46{0};   // v2 DIAG: ANY 0x46 dispatch with a kerfur Context
std::atomic<std::uint64_t> g_sampleCycles{0};
std::atomic<std::uint64_t> g_sampleCount{0};
std::atomic<std::uint64_t> g_seq{0};           // dispatch sequence -> 1/1024 RDTSC sampling
std::atomic<int>           g_liveCatchLogged{0};
std::atomic<int>           g_verbLogged{0};

constexpr int kLiveCatchCap = 16;  // log the first N name-matches richly, then just count.
constexpr int kVerbLogCap   = 64;  // v3: rich-log the first N VERB-comparison-index hits.

double g_tscGHz = 0.0;

inline void* PeekCode(void* stack) {
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(stack) + kFFrameCodeOff);
}

// The 8-byte name key at the operand cursor (ComparisonIndex + Number, little-endian);
// Display@8 is intentionally ignored -- the engine's own name lookup compares only
// these 8 bytes (measured sub_1412FDF90).
inline std::uint64_t PeekName8(void* stack) {
    void* code = PeekCode(stack);
    std::uint64_t k;
    std::memcpy(&k, code, sizeof(k));
    return k;
}

inline std::uint64_t PackFName(const R::FName& n) {
    return static_cast<std::uint32_t>(n.ComparisonIndex) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(n.Number)) << 32);
}

// v3 DIAGNOSTIC filter. v2 proved the FScriptName operand is {ComparisonIndex@0,
// DisplayIndex@4, Number@8} (shipping: CmpIdx==DispIdx -> low32==high32 in every catch),
// NOT the spike's {CmpIdx, Number, Display}. So the correct FName key is
// {ComparisonIndex = op[0], Number = op[8]} -- match THAT. Runs only enabled+resolved+GT.
template <int OP>
inline void RunKerfurDiag(void* ctx, void* stack) {
    // (1) NAME match on the CORRECT decode (0x45 only): op[0]=ComparisonIndex, op[8]=Number.
    if (OP == 0x45) {
        const std::uint32_t* op = reinterpret_cast<std::uint32_t*>(PeekCode(stack));
        const std::uint32_t opCmp = op[0];
        const std::uint32_t opNum = op[2];  // int32 #3 = Number@byte8
        const std::uint32_t vDropCmp  = static_cast<std::uint32_t>(g_verbDropProp.load(std::memory_order_relaxed)  & 0xffffffff);
        const std::uint32_t vSpawnCmp = static_cast<std::uint32_t>(g_verbSpawnKerf.load(std::memory_order_relaxed) & 0xffffffff);
        if (vDropCmp && (opCmp == vDropCmp || opCmp == vSpawnCmp)) {
            g_nameMatch.fetch_add(1, std::memory_order_relaxed);
            if (g_verbLogged.load(std::memory_order_relaxed) < kVerbLogCap) {
                const int n = g_verbLogged.fetch_add(1, std::memory_order_relaxed);
                if (n < kVerbLogCap) {
                    std::wstring clsName = R::ClassNameOf(ctx);
                    UE_LOGI("[gnatives_probe] *** VERB-HIT #%d: opcode=0x45 op[0]=CmpIdx=0x%x "
                            "op[1]=DispIdx=0x%x op[2]=Number=0x%x (%ls) Context.class=%ls ***",
                            n, opCmp, op[1], opNum,
                            (opCmp == vDropCmp) ? L"dropKerfurProp" : L"spawnKerfuro",
                            clsName.c_str());
                }
            }
        }
    }

    // (2) CLASS-GATED catch-all counter (no rich log -- v2 showed it fills with init noise).
    void* cls = R::ClassOf(ctx);
    if (!cls) return;
    void* bases[2] = {g_npcClass.load(std::memory_order_relaxed),
                      g_propClass.load(std::memory_order_relaxed)};
    if (!R::IsDescendantOfAny(cls, bases, 2)) return;
    if (OP == 0x45) g_kerfurCtx45.fetch_add(1, std::memory_order_relaxed);
    else            g_kerfurCtx46.fetch_add(1, std::memory_order_relaxed);
}

template <int OP>
std::uintptr_t __fastcall Wrapper(void* ctx, void* stack, void* result) {
    // The eternal cost the process pays on EVERY 0x45 dispatch forever (the swap is
    // never removed): read the enable flag first. Disabled fast-path = this load +
    // branch + tail-call (the solo-SP tax the plan S2.0 gate measures).
    const bool sample = (g_seq.fetch_add(1, std::memory_order_relaxed) & 1023) == 0;
    const std::uint64_t t0 = sample ? __rdtsc() : 0;

    // Count on BOTH modes so each run reports a GT dispatch rate. In DISABLED-tax
    // mode the sampled cost = IsGameThread + counter (a CONSERVATIVE UPPER BOUND on
    // the true fast path, which is just load+branch+tail-call); if even this bound
    // clears 0.1 ms/frame the real disabled tax does too.
    const bool gt = GT::IsGameThread();
    if (gt) g_countGT.fetch_add(1, std::memory_order_relaxed);
    else    g_countWorker.fetch_add(1, std::memory_order_relaxed);
    if (g_enabled && gt && g_resolved.load(std::memory_order_acquire))
        RunKerfurDiag<OP>(ctx, stack);

    if (sample) {
        g_sampleCycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        g_sampleCount.fetch_add(1, std::memory_order_relaxed);
    }

    return (OP == 0x46 ? g_origFinal : g_origVirtual)(ctx, stack, result);
}

// Resolve the two verb FNames + two classes on the GAME THREAD (StringToFName
// dispatches ProcessEvent -> GT-only; must NEVER run inside the wrapper). Posted
// from the dumper each second until everything resolves (handles BP-load timing).
void ResolveOnGameThread() {
    if (g_resolved.load(std::memory_order_relaxed)) return;

    void* npc  = g_npcClass.load(std::memory_order_relaxed);
    void* prop = g_propClass.load(std::memory_order_relaxed);
    if (!npc)  { npc  = R::FindClass(L"kerfurOmega_C");      if (npc)  g_npcClass.store(npc,  std::memory_order_relaxed); }
    if (!prop) { prop = R::FindClass(L"prop_kerfurOmega_C"); if (prop) g_propClass.store(prop, std::memory_order_relaxed); }

    std::uint64_t vDrop  = g_verbDropProp.load(std::memory_order_relaxed);
    std::uint64_t vSpawn = g_verbSpawnKerf.load(std::memory_order_relaxed);
    if (!vDrop)  { R::FName f = ue_wrap::fname_utils::StringToFName(L"dropKerfurProp"); if (f.ComparisonIndex) { vDrop  = PackFName(f); g_verbDropProp.store(vDrop,  std::memory_order_relaxed); } }
    if (!vSpawn) { R::FName f = ue_wrap::fname_utils::StringToFName(L"spawnKerfuro");   if (f.ComparisonIndex) { vSpawn = PackFName(f); g_verbSpawnKerf.store(vSpawn, std::memory_order_relaxed); } }

    if (npc && prop && vDrop && vSpawn) {
        g_resolved.store(true, std::memory_order_release);
        UE_LOGI("[gnatives_probe] filter RESOLVED: dropKerfurProp=0x%016llx spawnKerfuro=0x%016llx "
                "kerfurOmega_C=%p prop_kerfurOmega_C=%p -- name-first filter ARMED",
                (unsigned long long)vDrop, (unsigned long long)vSpawn, npc, prop);
    }
}

std::uintptr_t* ResolveGNatives() {
    const uintptr_t hit = ue_wrap::FindPattern(
        "4C 8D 0D ?? ?? ?? ?? 49 8B D7 0F B6 08 48 FF C0 49 89 47 20 8B C1 49 8B 4F 18 41 FF 14 C1");
    if (!hit) return nullptr;
    const std::int32_t rel = *reinterpret_cast<std::int32_t*>(hit + 3);
    return reinterpret_cast<std::uintptr_t*>(hit + 7 + rel);
}

bool ValidateTable(const std::uintptr_t* tbl) {
    uintptr_t base = 0, size = 0;
    ue_wrap::MainModuleRange(base, size);
    int inRange = 0;
    for (int i = 0; i < 256; ++i) {
        const std::uintptr_t p = tbl[i];
        if (p >= base && p < base + size) ++inRange;
    }
    return inRange >= 200;
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
    std::uint64_t lastGT = 0, lastWk = 0, lastCyc = 0, lastN = 0, lastNM = 0, lastCC = 0;
    for (;;) {
        ::Sleep(1000);

        if (!g_resolved.load(std::memory_order_acquire) && g_enabled)
            GT::Post([] { ResolveOnGameThread(); });

        const std::uint64_t gt = g_countGT.load(std::memory_order_relaxed);
        const std::uint64_t wk = g_countWorker.load(std::memory_order_relaxed);
        const std::uint64_t cyc = g_sampleCycles.load(std::memory_order_relaxed);
        const std::uint64_t n = g_sampleCount.load(std::memory_order_relaxed);
        const std::uint64_t nm = g_nameMatch.load(std::memory_order_relaxed);
        const std::uint64_t cc = g_classConfirmed.load(std::memory_order_relaxed);
        const std::uint64_t offgt = g_offGtMatch.load(std::memory_order_relaxed);

        const std::uint64_t dGT = gt - lastGT;
        const std::uint64_t dWk = wk - lastWk;
        const std::uint64_t dCyc = cyc - lastCyc;
        const std::uint64_t dN = n - lastN;
        const std::uint64_t dNM = nm - lastNM;
        const std::uint64_t dCC = cc - lastCC;
        lastGT = gt; lastWk = wk; lastCyc = cyc; lastN = n; lastNM = nm; lastCC = cc;

        const double avgCyc = dN ? (double(dCyc) / double(dN)) : 0.0;
        const double nsPerCall = (g_tscGHz > 0.0) ? (avgCyc / g_tscGHz) : 0.0;
        const double gtMsPerSec = (double(dGT) * nsPerCall) / 1e6;

        const std::uint64_t kc45 = g_kerfurCtx45.load(std::memory_order_relaxed);
        const std::uint64_t kc46 = g_kerfurCtx46.load(std::memory_order_relaxed);
        UE_LOGI("[gnatives_probe] 1s (%s): GT=%llu/s worker=%llu/s | avg=%.1f cyc/call "
                "(%.1f ns, %llu samp) | GT added=%.3f ms/s => %.4f ms/frame@120 "
                "%.4f ms/frame@60 (gate <=0.1) | nameMatch=+%llu kerfurCtx45(total)=%llu "
                "kerfurCtx46(total)=%llu resolved=%d",
                g_enabled ? "ENABLED-filter" : "DISABLED-tax",
                (unsigned long long)dGT, (unsigned long long)dWk, avgCyc, nsPerCall,
                (unsigned long long)dN, gtMsPerSec, gtMsPerSec / 120.0, gtMsPerSec / 60.0,
                (unsigned long long)dNM, (unsigned long long)kc45, (unsigned long long)kc46,
                g_resolved.load(std::memory_order_relaxed) ? 1 : 0);
        (void)dCC;
    }
}

}  // namespace

void Init() {
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    if (!coop::config::IsIniKeyTrue("gnatives_probe")) return;

    g_enabled = !coop::config::IsIniKeyTrue("gnatives_probe_disabled");

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

    // v2 DIAGNOSTIC: swap BOTH 0x45 (EX_LocalVirtualFunction) AND 0x46
    // (EX_LocalFinalFunction) -- v1 saw the verb on NEITHER (nameMatch=0), so we widen the
    // catch to find which opcode the real menu toggle actually uses (H1 vs H2 vs H3).
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

    UE_LOGI("[gnatives_probe] ARMED-v3 (%s): GNatives@%p [0x45]->%p [0x46]->%p (origV=%p origF=%p) "
            "tsc=%.2f GHz -- STEP 1.0 DIAG: class-gated kerfur-context catch on BOTH opcodes",
            g_enabled ? "ENABLED-filter" : "DISABLED-tax",
            (void*)g_gnatives, (void*)&Wrapper<0x45>, (void*)&Wrapper<0x46>,
            (void*)g_origVirtual, (void*)g_origFinal, g_tscGHz);

    std::thread(DumperThread).detach();
}

}  // namespace coop::dev::gnatives_probe
