// coop/element/object_scan_hub.cpp -- the one pass over the object index that every consumer
// shares, in place of a walk over the object array.
#include "coop/element/object_scan_hub.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::element::scan_hub {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
using steady_clock = std::chrono::steady_clock;

// Pass cadence. A full pass every tenth one re-reads every matched instance's identity, which a
// tail pass cannot: a key restored by a load after the birth is a change no notification reports.
constexpr auto    kPassCadence   = std::chrono::seconds(2);
constexpr int     kBackstopEvery = 10;
// Slice budget: ~1 ms of game-thread time per frame, with the clock checked every kSliceCheck
// items. An item costs a slot read, a flags read, a world climb and then the consumers' own
// match work (a key rendered per hit), five to ten microseconds, so the check is frequent
// enough that a slice overshoots the budget by a fraction, not by multiples.
constexpr int64_t kSliceBudgetUs = 1000;
constexpr int32_t kSliceCheck    = 32;
// A slot the game thread may not read: dying, or not yet handed over by the loading thread, or
// allocated with its class constructor still to run.
constexpr int32_t kUnreadable = R::slot_flags::Dying | R::slot_flags::NotYetReadable;

struct Row {
    Consumer c;
    size_t lastCount   = static_cast<size_t>(-1);
    int    stableScans = 0;
    bool   activeThisPass = false;   // EnsureResolved() succeeded at pass start
    bool   wasActive      = false;   // the previous pass's verdict, for the resolve edge
    bool   skippedByMutate = false;  // VOTVCOOP_HUB_SKIP drill control
};

// A birth the index reported whose class matched at least one consumer at the time.
struct Birth {
    void*   obj;
    int32_t idx;
    void*   cls;
};
// One entry of a pass's work list, with the consumer bitmask taken when the list was built.
struct Item {
    void*    obj;
    int32_t  idx;
    uint64_t bits;
};

std::vector<Row>      g_rows;
std::vector<Consumer> g_pendingRegs;   // registrations arriving mid-pass join the next pass
bool                  g_inPass = false;
bool                  g_observerSet = false;

// Every class the index holds, with the consumers whose predicate accepted it. Filled by the
// index's class callbacks; `g_reclassify` names the rows whose verdict must be re-taken over
// every class before the next pass (a new row, or a row whose classes just resolved).
std::unordered_map<void*, uint64_t> g_classBits;
uint64_t                            g_reclassify = 0;
std::vector<Birth>                  g_births;   // since the last pass start

// Pass state.
std::vector<Item> g_work;
size_t            g_cursor    = 0;
bool              g_passFull  = false;
uint32_t          g_passGen   = 0;
steady_clock::time_point g_passStart{};
int               g_passSlices = 0;

// Cross-pass state.
bool                     g_everFull      = false;
int                      g_sinceFull     = 0;    // completed passes since the last full one
bool                     g_forceFullOnce = false;  // dev drill: ONE forced full, settle untouched
steady_clock::time_point g_nextPassDue = steady_clock::time_point::min();

bool ScanDiagOn() {
    static const bool on = [] {
        const char* v = std::getenv("VOTVCOOP_SCAN_DIAG");
        return v && v[0] == '1';
    }();
    return on;
}

// Parity-drill mutate control: VOTVCOOP_HUB_SKIP=<name> silently drops one consumer from the
// pass -- the drill must observe parity turn RED for exactly that consumer once, proving the
// instrument can see a miss, before any green run counts.
const char* HubSkipName() {
    static const char* v = std::getenv("VOTVCOOP_HUB_SKIP");
    return (v && v[0]) ? v : nullptr;
}

uint64_t RowBit(size_t ci) { return 1ull << ci; }

uint64_t AllRowsMask() {
    uint64_t m = 0;
    for (size_t ci = 0; ci < g_rows.size(); ++ci) m |= RowBit(ci);
    return m;
}

// The consumers in `rowMask` whose class-pure predicate accepts `anyObj`'s class.
uint64_t ClassifyRows(void* anyObj, uint64_t rowMask) {
    uint64_t bits = 0;
    for (size_t ci = 0; ci < g_rows.size(); ++ci) {
        if (!(rowMask & RowBit(ci))) continue;
        if (g_rows[ci].c.IsInstance(anyObj)) bits |= RowBit(ci);
    }
    return bits;
}

// The index's class callbacks.
void OnClassAppeared(void*, void* cls, void* firstObj) {
    g_classBits[cls] = ClassifyRows(firstObj, AllRowsMask());
}
void OnClassGone(void*, void* cls) { g_classBits.erase(cls); }
void OnObjectCreated(void*, void* obj, void* cls, int32_t idx) {
    auto it = g_classBits.find(cls);
    if (it != g_classBits.end() && it->second) g_births.push_back(Birth{obj, idx, cls});
}

void EnsureObserver() {
    if (g_observerSet) return;
    g_observerSet = true;
    OI::SetObserver(OI::Observer{nullptr, &OnClassAppeared, &OnClassGone, &OnObjectCreated});
}

// Re-take the verdict of the rows in `mask` over every class the index holds.
void Reclassify(uint64_t mask) {
    struct Ctx { uint64_t mask; };
    Ctx ctx{mask};
    OI::ForEachClass([](void* c, void* cls, void* anyInstance) {
        const uint64_t mask = static_cast<Ctx*>(c)->mask;
        uint64_t& bits = g_classBits[cls];
        bits = (bits & ~mask) | ClassifyRows(anyInstance, mask);
    }, &ctx);
}

bool AnyUnsettled() {
    for (const Row& r : g_rows) {
        if (r.skippedByMutate) continue;
        if (r.stableScans < r.c.settleScans) return true;
    }
    return false;
}

void AdoptPendingRegistrations() {
    for (const Consumer& c : g_pendingRegs) {
        if (g_rows.size() >= 64) {
            UE_LOGW("scan_hub: consumer '%s' REJECTED -- bitmask capacity (64) reached", c.name);
            continue;
        }
        Row r; r.c = c;
        if (const char* skip = HubSkipName(); skip && std::strcmp(skip, c.name) == 0) {
            r.skippedByMutate = true;
            UE_LOGW("scan_hub: consumer '%s' SKIPPED by VOTVCOOP_HUB_SKIP (parity mutate drill)", c.name);
        }
        g_rows.push_back(r);
        g_reclassify |= RowBit(g_rows.size() - 1);
        UE_LOGI("scan_hub: consumer '%s' registered (settleScans=%d, %zu total)",
                c.name, c.settleScans, g_rows.size());
    }
    g_pendingRegs.clear();
}

// Start a pass if one can. False when nothing is registered, the index is not yet seeded, no
// world is current (mid-transition: gens flip in adjacent ticks, so waiting one tick is the
// cheap correct move), or no consumer resolved.
bool StartPass() {
    AdoptPendingRegistrations();
    if (g_rows.empty()) return false;
    if (!OI::IsSeeded()) return false;
    if (ue_wrap::world_identity::CurrentWorld() == nullptr) return false;

    uint64_t activeMask = 0;
    for (size_t ci = 0; ci < g_rows.size(); ++ci) {
        Row& r = g_rows[ci];
        r.activeThisPass = !r.skippedByMutate && r.c.EnsureResolved();
        // A row whose classes just resolved took its verdicts while they were null: re-take them.
        if (r.activeThisPass && !r.wasActive) g_reclassify |= RowBit(ci);
        r.wasActive = r.activeThisPass;
        if (r.activeThisPass) activeMask |= RowBit(ci);
    }
    if (activeMask == 0) return false;
    if (g_reclassify) {
        Reclassify(g_reclassify);
        g_reclassify = 0;
    }

    g_passFull = !g_everFull || AnyUnsettled() || (g_sinceFull >= kBackstopEvery) || g_forceFullOnce;
    g_forceFullOnce = false;

    g_work.clear();
    if (g_passFull) {
        // Every live instance of every class some active consumer accepted.
        struct Ctx { uint64_t bits; };
        for (const auto& kv : g_classBits) {
            Ctx ctx{kv.second & activeMask};
            if (!ctx.bits) continue;
            OI::ForEachInstance(kv.first, [](void* c, void* obj, int32_t idx) {
                g_work.push_back(Item{obj, idx, static_cast<Ctx*>(c)->bits});
            }, &ctx);
        }
    } else {
        // The births since the last pass, judged by the verdicts as they stand now.
        for (const Birth& b : g_births) {
            auto it = g_classBits.find(b.cls);
            if (it == g_classBits.end()) continue;
            const uint64_t bits = it->second & activeMask;
            if (bits) g_work.push_back(Item{b.obj, b.idx, bits});
        }
    }
    g_births.clear();

    for (Row& r : g_rows)
        if (r.activeThisPass) r.c.OnPassBegin(r.c.ctx, g_passFull);

    g_passGen    = ue_wrap::world_identity::Generation();
    g_cursor     = 0;
    g_passStart  = steady_clock::now();
    g_passSlices = 0;
    g_inPass     = true;
    return true;
}

void AbortPass(const char* why) {
    // Scratch is cleared by the next OnPassBegin; OnPassComplete is never called for an aborted
    // pass, so no index moves. The next pass is full: whatever invalidated us invalidated the
    // births list's meaning too.
    UE_LOGI("scan_hub: pass ABORTED (%s) after %d slice(s) at item %zu/%zu -- next pass full",
            why, g_passSlices, g_cursor, g_work.size());
    g_inPass = false;
    g_work.clear();
    for (Row& r : g_rows) r.stableScans = 0;  // demand full passes until re-settled
    g_nextPassDue = steady_clock::now();       // retry promptly
}

void CompletePass() {
    const uint32_t gen = g_passGen;
    for (Row& r : g_rows) {
        if (!r.activeThisPass) continue;
        const size_t count = r.c.OnPassComplete(r.c.ctx, g_passFull, gen);
        // Settle feed: a count of zero never settles, and any change resets the run.
        if (count > 0 && count == r.lastCount) {
            if (r.stableScans < r.c.settleScans) ++r.stableScans;
        } else if (count != r.lastCount) {
            r.stableScans = 0;
        }
        r.lastCount = count;
    }
    if (g_passFull) { g_sinceFull = 0; g_everFull = true; } else ++g_sinceFull;
    g_inPass = false;
    const auto durUs = std::chrono::duration_cast<std::chrono::microseconds>(
                           steady_clock::now() - g_passStart).count();
    if (ScanDiagOn()) {
        UE_LOGI("[SCAN-DIAG] hub pass mode=%s items=%zu slices=%d dur=%lldus classes=%zu",
                g_passFull ? "full" : "tail", g_work.size(), g_passSlices,
                static_cast<long long>(durUs), g_classBits.size());
    }
    g_work.clear();
    // Cadence: the next pass is due 2 s after this one STARTED, but never before it completed --
    // max(2 s, duration), so a pass longer than the cadence runs back-to-back.
    const auto due = g_passStart + kPassCadence;
    const auto now = steady_clock::now();
    g_nextPassDue = (due > now) ? due : now;
}

// One slice of the active pass. Returns true if the pass completed inside this slice.
bool RunSlice() {
    ++g_passSlices;
    if (ue_wrap::world_identity::Generation() != g_passGen) { AbortPass("world-gen flip"); return false; }

    void* const world = ue_wrap::world_identity::CurrentWorld();
    const auto t0 = steady_clock::now();
    int32_t sinceCheck = 0;
    while (g_cursor < g_work.size()) {
        const Item& it = g_work[g_cursor++];
        // The list was built at pass start; the slot must still hold the object, the object must
        // be one the game thread may read, and it must belong to the world the game is running:
        // during the old and new world's coexistence after a travel, an actor whose slot says it
        // is live can still be the DYING world's, and the pass must not re-admit it under the
        // current gen stamp. A null WorldOf() means "not world-scoped", which no actor a consumer
        // indexes ever is.
        if (R::ObjectAt(it.idx) != it.obj) continue;
        if (R::SlotFlags(it.idx) & kUnreadable) continue;
        if (ue_wrap::world_identity::WorldOf(it.obj) != world) continue;
        for (size_t ci = 0; ci < g_rows.size(); ++ci) {
            if (it.bits & RowBit(ci)) g_rows[ci].c.OnMatch(g_rows[ci].c.ctx, it.obj);
        }
        if (++sinceCheck >= kSliceCheck) {
            sinceCheck = 0;
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                steady_clock::now() - t0).count();
            if (us >= kSliceBudgetUs) return false;  // budget spent -- resume next tick
        }
    }
    CompletePass();
    return true;
}

}  // namespace

void Register(const Consumer& c) {
    UE_ASSERT_GAME_THREAD("scan_hub::Register");
    EnsureObserver();
    if (g_rows.size() + g_pendingRegs.size() >= 64) {
        UE_LOGW("scan_hub: consumer '%s' REJECTED -- bitmask capacity (64) reached", c.name);
        return;
    }
    if (g_inPass) {
        // Mid-pass registration joins at the next pass start (the work list's bitmask indices
        // must stay stable for the life of a pass).
        g_pendingRegs.push_back(c);
        UE_LOGI("scan_hub: consumer '%s' registration QUEUED (pass active)", c.name);
        return;
    }
    g_pendingRegs.push_back(c);
    AdoptPendingRegistrations();
}

void Tick() {
    UE_ASSERT_GAME_THREAD("scan_hub::Tick");
    if (g_inPass) {
        RunSlice();
        return;
    }
    if (steady_clock::now() < g_nextPassDue) return;
    if (StartPass()) RunSlice();
}

void ForceSyncFullPass() {
    // Parity drill mode A: one complete FULL pass inside this game-thread call, unsliced, so the
    // caller can compare indexes against an independent probe walk with zero staleness. Settle
    // counters are NOT touched: resetting them here drags a train of re-settle full passes behind
    // the forced pass and contaminates the numeric gate. One flag = one extra full, nothing else.
    if (g_inPass) AbortPass("ForceSyncFullPass preempt");
    g_forceFullOnce = true;
    if (!StartPass()) return;
    while (g_inPass) {
        // RunSlice re-checks validity between "slices"; budget still applies per call but we
        // loop to completion synchronously.
        if (RunSlice()) break;
        if (!g_inPass) break;  // aborted
    }
}

bool DebugConsumerSettled(const char* name) {
    for (const Row& r : g_rows) {
        if (std::strcmp(r.c.name, name) == 0) return r.stableScans >= r.c.settleScans;
    }
    return false;
}

size_t DebugConsumerCount(const char* name) {
    for (const Row& r : g_rows) {
        if (std::strcmp(r.c.name, name) == 0)
            return (r.lastCount == static_cast<size_t>(-1)) ? SIZE_MAX : r.lastCount;
    }
    return SIZE_MAX;
}

}  // namespace coop::element::scan_hub
