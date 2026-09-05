// coop/dev/gnatives_probe.cpp -- see coop/dev/gnatives_probe.h. The probe-first gate for the
// permanent blueprint-VM dispatch substrate: it runs the same name-first filter shape the
// real wrapper ships (factored into the diagnostic filter below so it lifts into ue_wrap), so
// the per-frame measurement is of the real prologue. It measures, in one game run: the live
// catch and positive control (swap the local-virtual-call handler only, resolve the two
// kerfur verb names and classes on the game thread once, then per dispatch do a game-thread
// check, an 8-byte name compare (a raw name match proves the flip verb dispatched,
// independent of the class filter) and a class confirm, logging the first hits with the
// operand bytes, the context class and the thread); the cost, the enabled path against the
// disabled fast-path tax (the eternal solo cost of a never-removed swap: one relaxed load, a
// branch and a tail call), sampled by the cycle counter and reported per frame; the second
// verb family, the desk, drive and laptop verbs, all measured as local virtual calls (the
// local-final-call operand is a packed function pointer, not a name, so a name compare there
// reads garbage); and the signal-store sizes every 30 s (row counts and per-row image-blob
// statistics for the deck list and the two databases). Throwaway diagnostics; not shipped.

#include "coop/dev/gnatives_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sig_scan.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/world/economy.h"

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

// The native-handler ABI: exec(context, frame, result). The dispatcher ignores the return, but
// the local-virtual handler does return a value, so it is preserved.
using ExecFn = std::uintptr_t(__fastcall*)(void* ctx, void* stack, void* result);

// The frame's code cursor. At wrapper entry for the local-virtual-call opcode the cursor
// points at the 12-byte script-name operand (the comparison index, the display index, the
// number), measured live: the comparison and display indices are equal in shipping, and the
// number is zero for the clean verb names. Match the comparison index and the number, not
// the raw first 8 bytes.
constexpr std::size_t kFFrameCodeOff = 0x20;

std::uintptr_t* g_gnatives = nullptr;  // GNatives[256], the exec-handler table base.
ExecFn g_origVirtual = nullptr;        // GNatives[0x45] original.
ExecFn g_origFinal   = nullptr;        // GNatives[0x46] original.

bool g_enabled = true;  // false via ini gnatives_probe_disabled=1 -> measure the pure tax.

// The filter's resolved constants (set once on the game thread; read relaxed per dispatch). A
// packed name is the comparison index in the low word and the number in the high word; 0 is
// unresolved.
std::atomic<std::uint64_t> g_verbDropProp{0};   // dropKerfurProp (turn-off verb)
std::atomic<std::uint64_t> g_verbSpawnKerf{0};  // spawnKerfuro    (turn-on verb)
std::atomic<void*> g_npcClass{nullptr};         // kerfurOmega_C
std::atomic<void*> g_propClass{nullptr};        // prop_kerfurOmega_C
std::atomic<bool>  g_resolved{false};

// The counters (relaxed atomics; the dumper reads deltas once a second).
std::atomic<std::uint64_t> g_countGT{0};       // 0x45 dispatches on the game thread
std::atomic<std::uint64_t> g_countWorker{0};   // 0x45 dispatches off the game thread
std::atomic<std::uint64_t> g_nameMatch{0};     // POSITIVE CONTROL: raw name==verb (pre-class)
std::atomic<std::uint64_t> g_classConfirmed{0};// name-match AND class descends kerfur family
std::atomic<std::uint64_t> g_offGtMatch{0};    // name-match seen OFF the GT (tripwire)
std::atomic<std::uint64_t> g_kerfurCtx45{0};   // ANY 0x45 dispatch with a kerfur Context
std::atomic<std::uint64_t> g_kerfurCtx46{0};   // ANY 0x46 dispatch with a kerfur Context
std::atomic<std::uint64_t> g_sampleCycles{0};
std::atomic<std::uint64_t> g_sampleCount{0};
std::atomic<std::uint64_t> g_seq{0};           // dispatch sequence -> 1/1024 RDTSC sampling
std::atomic<int>           g_liveCatchLogged{0};
std::atomic<int>           g_verbLogged{0};

constexpr int kLiveCatchCap = 16;  // log the first N name-matches richly, then just count.
constexpr int kVerbLogCap   = 64;  // rich-log the first N VERB-comparison-index hits.

// The second verb family: the desk, drive and laptop verb matchers (all local virtual calls,
// measured).
struct F2Verb {
    const wchar_t* name;
    std::atomic<std::uint64_t> packed{0};  // FName {CmpIdx | Number<<32}; 0 = unresolved
    std::atomic<std::uint64_t> hits{0};    // GT 0x45 name-matches
};
F2Verb g_f2[] = {
    {L"saveSignal"}, {L"deleteSignal"}, {L"addSignal"}, {L"removeSignal"},
    {L"sortSignal"}, {L"comp_uploadData"}, {L"putDriveIn"}, {L"drivePulledOut"},
    {L"upd"},
};
constexpr int kF2Count = sizeof(g_f2) / sizeof(g_f2[0]);
std::atomic<bool> g_f2Resolved{false};
std::atomic<int>  g_f2Logged{0};
constexpr int kF2LogCap = 48;  // rich-log the first N family-2 hits (class discriminates)

double g_tscGHz = 0.0;

inline void* PeekCode(void* stack) {
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(stack) + kFFrameCodeOff);
}

// The 8-byte name key at the operand cursor (the comparison index and the number); the
// display index is ignored, since the engine's own name lookup compares only these 8 bytes.
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

// The diagnostic filter. The script-name operand is the comparison index, the display index
// and the number, so the correct key is the comparison index at the cursor and the number 8
// bytes in. Runs only enabled, resolved and on the game thread.
template <int OP>
inline void RunKerfurDiag(void* ctx, void* stack) {
    // The name match on the correct decode (the local-virtual-call opcode only).
    if (OP == 0x45) {
        const std::uint32_t* op = reinterpret_cast<std::uint32_t*>(PeekCode(stack));
        const std::uint32_t opCmp = op[0];
        const std::uint32_t opNum = op[2];  // int32 #3 = Number@byte8
        const std::uint32_t vDropCmp  = static_cast<std::uint32_t>(g_verbDropProp.load(std::memory_order_relaxed)  & 0xffffffff);
        const std::uint32_t vSpawnCmp = static_cast<std::uint32_t>(g_verbSpawnKerf.load(std::memory_order_relaxed) & 0xffffffff);
        // The second family: a linear scan of the desk, drive and laptop verb indices (a zero skip
        // guards unresolved slots; index 0 is the None name, never a real call).
        for (int i = 0; i < kF2Count; ++i) {
            const std::uint32_t cmp = static_cast<std::uint32_t>(
                g_f2[i].packed.load(std::memory_order_relaxed) & 0xffffffff);
            if (!cmp || opCmp != cmp) continue;
            g_f2[i].hits.fetch_add(1, std::memory_order_relaxed);
            if (g_f2Logged.load(std::memory_order_relaxed) < kF2LogCap) {
                const int n = g_f2Logged.fetch_add(1, std::memory_order_relaxed);
                if (n < kF2LogCap) {
                    std::wstring clsName = R::ClassNameOf(ctx);
                    UE_LOGI("[gnatives_probe] F2-HIT #%d: %ls Number=0x%x Context.class=%ls",
                            n, g_f2[i].name, op[2], clsName.c_str());
                }
            }
            break;
        }
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

    // The class-gated catch-all counter (no rich log; it fills with init noise).
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
    // The eternal cost the process pays on every dispatch of this opcode, since the swap is never
    // removed: read the enable flag first. The disabled fast path is this load, a branch and a
    // tail call (the solo tax the gate measures).
    const bool sample = (g_seq.fetch_add(1, std::memory_order_relaxed) & 1023) == 0;
    const std::uint64_t t0 = sample ? __rdtsc() : 0;

    // Count in both modes, so each run reports a game-thread dispatch rate. In the disabled-tax
    // mode the sampled cost is the thread check plus the counter, a conservative upper bound on
    // the true fast path; if even this bound clears the budget, the real tax does too.
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

// Resolve the two verb names and two classes on the game thread (the name conversion
// dispatches ProcessEvent, so game thread only; it must never run inside the wrapper).
// Posted from the dumper each second until everything resolves.
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

    // The second-family names (the string-to-name conversion adds, so it resolves on the first
    // pass; the name table is global, so our index equals the load-patched bytecode operand's).
    if (!g_f2Resolved.load(std::memory_order_relaxed)) {
        bool all = true;
        for (int i = 0; i < kF2Count; ++i) {
            if (g_f2[i].packed.load(std::memory_order_relaxed)) continue;
            R::FName f = ue_wrap::fname_utils::StringToFName(g_f2[i].name);
            if (f.ComparisonIndex) g_f2[i].packed.store(PackFName(f), std::memory_order_relaxed);
            else all = false;
        }
        if (all) {
            g_f2Resolved.store(true, std::memory_order_release);
            UE_LOGI("[gnatives_probe] FAMILY-2 RESOLVED: %d desk/drive/laptop verb matchers armed",
                    kF2Count);
        }
    }
}

// The sizes dump, posted to the game thread every 30 s: the three signal stores' row counts
// and the per-row image-blob statistics, the packet-budget measurement.
void DumpSignalStoreSizes() {
    namespace SD = ue_wrap::signal_dynamic;

    struct Stats { int rows = -1; int imgMin = 0, imgMax = 0; long long imgSum = 0; };
    auto walk = [](const std::uint8_t* base, std::int32_t off) -> Stats {
        Stats s;
        if (!base || off < 0) return s;
        const auto* arr = reinterpret_cast<const std::int32_t*>(base + off + 8);  // TArray.Num
        const std::uint8_t* data = *reinterpret_cast<std::uint8_t* const*>(base + off);
        const std::int32_t num = arr[0];
        if (num < 0 || num > 4096) return s;
        s.rows = num;
        if (!data) return s;
        s.imgMin = num ? INT32_MAX : 0;
        for (std::int32_t i = 0; i < num; ++i) {
            const std::int32_t imgNum = *reinterpret_cast<const std::int32_t*>(
                data + std::size_t(i) * SD::kStride + SD::kOff_image + 8);
            const std::int32_t n = (imgNum >= 0 && imgNum < (64 << 20)) ? imgNum : 0;
            if (n < s.imgMin) s.imgMin = n;
            if (n > s.imgMax) s.imgMax = n;
            s.imgSum += n;
        }
        return s;
    };

    // The deck list: the gamemode's saved signals.
    static void* gmCls = nullptr;
    static std::int32_t offDeck = -1;
    if (!gmCls) gmCls = R::FindClass(L"mainGamemode_C");
    if (gmCls && offDeck < 0) offDeck = R::FindPropertyOffset(gmCls, L"savedSignals_0");
    void* gm = nullptr;
    if (gmCls) {
        for (void* obj : R::FindObjectsByClass(L"mainGamemode_C"))
            if (obj && R::IsLive(obj)) { gm = obj; break; }
    }
    const Stats deck = walk(static_cast<const std::uint8_t*>(gm), offDeck);

    // The meadow database and the processed database: the save slot's two saved-signal arrays.
    void* ss = ue_wrap::economy::SaveSlotPtr();
    static std::int32_t offMeadow = -1, offComp = -1;
    if (ss && offMeadow < 0) {
        void* ssCls = R::ClassOf(ss);
        if (ssCls) {
            offMeadow = R::FindPropertyOffset(ssCls, L"savedSignals_0");
            offComp   = R::FindPropertyOffset(ssCls, L"savedSignals_comp_0");
        }
    }
    const Stats meadow = walk(static_cast<const std::uint8_t*>(ss), offMeadow);
    const Stats comp   = walk(static_cast<const std::uint8_t*>(ss), offComp);

    auto avg = [](const Stats& s) -> long long { return s.rows > 0 ? s.imgSum / s.rows : 0; };
    UE_LOGI("[gnatives_probe] SIZES (stride 0x70): deck=%d rows (img min/max/avg=%d/%d/%lld B) "
            "| meadowDB=%d (img %d/%d/%lld) | compDB=%d (img %d/%d/%lld) "
            "[offs deck=0x%X meadow=0x%X comp=0x%X]",
            deck.rows, deck.imgMin, deck.imgMax, avg(deck),
            meadow.rows, meadow.imgMin, meadow.imgMax, avg(meadow),
            comp.rows, comp.imgMin, comp.imgMax, avg(comp),
            offDeck, offMeadow, offComp);
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
    int tick = 0;
    for (;;) {
        ::Sleep(1000);
        ++tick;

        if ((!g_resolved.load(std::memory_order_acquire) ||
             !g_f2Resolved.load(std::memory_order_acquire)) && g_enabled)
            GT::Post([] { ResolveOnGameThread(); });

        // The sizes dump every 30 s (a game-thread task; object walks are game-thread only).
        if (g_enabled && (tick % 30) == 0)
            GT::Post([] { DumpSignalStoreSizes(); });

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

        // The second-family totals once per 10 s, only when any counter is non-zero, so
        // ambient-quiet runs stay quiet in the log.
        if ((tick % 10) == 0) {
            std::uint64_t total = 0;
            for (int i = 0; i < kF2Count; ++i) total += g_f2[i].hits.load(std::memory_order_relaxed);
            if (total) {
                UE_LOGI("[gnatives_probe] F2 totals: saveSignal=%llu deleteSignal=%llu addSignal=%llu "
                        "removeSignal=%llu sortSignal=%llu comp_uploadData=%llu putDriveIn=%llu "
                        "drivePulledOut=%llu upd=%llu",
                        (unsigned long long)g_f2[0].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[1].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[2].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[3].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[4].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[5].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[6].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[7].hits.load(std::memory_order_relaxed),
                        (unsigned long long)g_f2[8].hits.load(std::memory_order_relaxed));
            }
        }
    }
}

}  // namespace

void Init() {
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    if (!coop::config::ResolveFlag(::coop::config_registry::rows::gnatives_probe)) return;

    g_enabled = !coop::config::ResolveFlag(::coop::config_registry::rows::gnatives_probe_disabled);

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

    // Swap both the local-virtual-call and the local-final-call handlers, so the catch shows
    // which opcode the real menu toggle uses.
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

    UE_LOGI("[gnatives_probe] ARMED-v4 (%s): GNatives@%p [0x45]->%p [0x46]->%p (origV=%p origF=%p) "
            "tsc=%.2f GHz -- v4: kerfur family + 9 desk/drive/laptop matchers + SIZES dump",
            g_enabled ? "ENABLED-filter" : "DISABLED-tax",
            (void*)g_gnatives, (void*)&Wrapper<0x45>, (void*)&Wrapper<0x46>,
            (void*)g_origVirtual, (void*)g_origFinal, g_tscGHz);

    std::thread(DumperThread).detach();
}

}  // namespace coop::dev::gnatives_probe
