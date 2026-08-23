// coop/element/object_scan_hub.cpp -- the shared sliced GUObjectArray pass (R-2).
// Design of record + /qf round map:
// research/findings/architecture-audits/votv-shared-scan-hub-R2-DESIGN-2026-08-23.md
#include "coop/element/object_scan_hub.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::element::scan_hub {
namespace {

namespace R = ue_wrap::reflection;
using steady_clock = std::chrono::steady_clock;

// Proven tuning carried over verbatim from the retired SettledObjectScan (L5 take-3): at the
// 2 s pass cadence, a full backstop every 30 completed passes ~ 60 s.
constexpr auto    kPassCadence   = std::chrono::seconds(2);
constexpr int     kBackstopEvery = 30;
// Slice budget: ~1 ms of GT time per frame, clock checked every kSliceCheck objects. Priced in
// the design (~17 ns/object dev, ~43 ns/object on the field reporter's machine).
constexpr int64_t kSliceBudgetUs = 1000;
constexpr int32_t kSliceCheck    = 4096;

struct Row {
    Consumer c;
    // Settle state (semantics verbatim from SettledObjectScan::End).
    size_t lastCount   = static_cast<size_t>(-1);
    int    stableScans = 0;
    bool   activeThisPass = false;   // EnsureResolved() succeeded at pass start
    bool   skippedByMutate = false;  // VOTVCOOP_HUB_SKIP drill control
};

struct MemoEntry {
    uint64_t bits;      // consumer match bitmask
    int32_t  clsIdx;    // the UClass object's own GUObjectArray slot
    int32_t  clsSerial; // its slot serial at memo time
};

std::vector<Row>  g_rows;
std::vector<Consumer> g_pendingRegs;   // registrations arriving mid-pass join the next pass
bool              g_inPass = false;

// Pass state.
bool      g_passFull   = false;
int32_t   g_passBegin  = 0;
int32_t   g_passCursor = 0;
int32_t   g_passEnd    = 0;
uint32_t  g_passGen    = 0;
steady_clock::time_point g_passStart{};
int       g_passSlices = 0;
std::unordered_map<void*, MemoEntry> g_memo;   // pass-scoped: cleared at every pass start

// Cross-pass state.
int32_t                  g_tailCursor  = 0;    // objects >= this are "new" for the next tail pass
int                      g_sinceFull   = 0;    // completed passes since the last full one
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

bool AnyUnsettled() {
    for (const Row& r : g_rows) {
        if (r.skippedByMutate) continue;
        if (r.stableScans < r.c.settleScans) return true;
    }
    return false;
}

void AdoptPendingRegistrations() {
    for (const Consumer& c : g_pendingRegs) {
        Row r; r.c = c;
        if (const char* skip = HubSkipName(); skip && std::strcmp(skip, c.name) == 0) {
            r.skippedByMutate = true;
            UE_LOGW("scan_hub: consumer '%s' SKIPPED by VOTVCOOP_HUB_SKIP (parity mutate drill)", c.name);
        }
        g_rows.push_back(r);
        UE_LOGI("scan_hub: consumer '%s' registered (settleScans=%d, %zu total)",
                c.name, c.settleScans, g_rows.size());
    }
    g_pendingRegs.clear();
}

// Start a pass if one is due. Returns false when no pass could start (no world, no consumers).
bool StartPass() {
    AdoptPendingRegistrations();
    if (g_rows.empty()) return false;
    // No current world (mid-transition): don't start -- gens flip in adjacent ticks (measured:
    // exactly two flips then stable), so waiting one tick is the cheap correct move.
    if (ue_wrap::world_identity::CurrentWorld() == nullptr) return false;

    const int32_t n = R::NumObjects();
    // Shrink below the tail cursor = a purge freed slots we think we scanned -> full (the
    // NextRange rule, preserved).
    const bool shrunk = (n < g_tailCursor);
    g_passFull = AnyUnsettled() || shrunk || (g_sinceFull >= kBackstopEvery);

    int activeCount = 0;
    for (Row& r : g_rows) {
        r.activeThisPass = !r.skippedByMutate && r.c.EnsureResolved();
        if (r.activeThisPass) {
            r.c.OnPassBegin(r.c.ctx, g_passFull);
            ++activeCount;
        }
    }
    if (activeCount == 0) return false;

    g_passGen    = ue_wrap::world_identity::Generation();
    g_passBegin  = g_passFull ? 0 : g_tailCursor;
    g_passCursor = g_passBegin;
    g_passEnd    = n;
    g_passStart  = steady_clock::now();
    g_passSlices = 0;
    g_memo.clear();
    g_inPass     = true;
    return true;
}

void AbortPass(const char* why) {
    // Scratch is cleared by the next OnPassBegin; OnPassComplete is never called for an
    // aborted pass, so no index moves. Force the next pass FULL (whatever invalidated us also
    // invalidated the tail cursor's meaning).
    UE_LOGI("scan_hub: pass ABORTED (%s) after %d slice(s) at cursor %d/%d -- next pass full",
            why, g_passSlices, g_passCursor, g_passEnd);
    g_inPass = false;
    g_tailCursor = 0;
    for (Row& r : g_rows) r.stableScans = 0;  // demand full passes until re-settled
    g_nextPassDue = steady_clock::now();       // retry promptly
}

void CompletePass() {
    const uint32_t gen = g_passGen;
    for (Row& r : g_rows) {
        if (!r.activeThisPass) continue;
        const size_t count = r.c.OnPassComplete(r.c.ctx, g_passFull, gen);
        // Settle feed -- verbatim SettledObjectScan::End semantics (zero never settles; any
        // change resets; see the retired component's 18:41 rationale, preserved).
        if (count > 0 && count == r.lastCount) {
            if (r.stableScans < r.c.settleScans) ++r.stableScans;
        } else if (count != r.lastCount) {
            r.stableScans = 0;
        }
        r.lastCount = count;
    }
    g_tailCursor = g_passEnd;
    if (g_passFull) g_sinceFull = 0; else ++g_sinceFull;
    g_inPass = false;
    const auto durUs = std::chrono::duration_cast<std::chrono::microseconds>(
                           steady_clock::now() - g_passStart).count();
    if (ScanDiagOn()) {
        UE_LOGI("[SCAN-DIAG] hub pass mode=%s range=%d slices=%d dur=%lldus",
                g_passFull ? "full" : "tail", g_passEnd - g_passBegin,
                g_passSlices, static_cast<long long>(durUs));
    }
    // Cadence: next pass due 2 s after this one STARTED, but never before it completed
    // (max(2s, duration+eps) from the design -- a pass longer than the cadence back-to-backs).
    const auto due = g_passStart + kPassCadence;
    const auto now = steady_clock::now();
    g_nextPassDue = (due > now) ? due : now;
}

// One slice of the active pass. Returns true if the pass completed inside this slice.
bool RunSlice() {
    ++g_passSlices;
    // Validity checks between slices (the pass spans frames; the array does not stand still).
    if (ue_wrap::world_identity::Generation() != g_passGen) { AbortPass("world-gen flip"); return false; }
    const int32_t curN = R::NumObjects();
    if (curN < g_passCursor) { AbortPass("array shrank below cursor"); return false; }
    const int32_t end = (g_passEnd < curN) ? g_passEnd : curN;

    const auto t0 = steady_clock::now();
    int32_t sinceCheck = 0;
    while (g_passCursor < end) {
        const int32_t i = g_passCursor++;
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        uint64_t bits;
        auto it = g_memo.find(cls);
        if (it != g_memo.end() &&
            R::IsLiveByIndex(cls, it->second.clsIdx) &&
            R::SlotSerial(it->second.clsIdx) == it->second.clsSerial) {
            bits = it->second.bits;
        } else {
            // Memo miss (or a recycled UClass* failed the serial re-verify): run every active
            // consumer's class-pure predicate ONCE for this class.
            bits = 0;
            for (size_t ci = 0; ci < g_rows.size(); ++ci) {
                if (!g_rows[ci].activeThisPass) continue;
                if (g_rows[ci].c.IsInstance(obj)) bits |= (1ull << ci);
            }
            const int32_t clsIdx = R::InternalIndexOf(cls);
            g_memo[cls] = MemoEntry{bits, clsIdx, R::SlotSerial(clsIdx)};
        }
        if (bits) {
            for (size_t ci = 0; ci < g_rows.size(); ++ci) {
                if (bits & (1ull << ci)) g_rows[ci].c.OnMatch(g_rows[ci].c.ctx, obj);
            }
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
    if (g_rows.size() + g_pendingRegs.size() >= 64) {
        UE_LOGW("scan_hub: consumer '%s' REJECTED -- bitmask capacity (64) reached", c.name);
        return;
    }
    if (g_inPass) {
        // Mid-pass registration joins at the next pass start (the memo's bitmask indices must
        // stay stable for the life of a pass).
        g_pendingRegs.push_back(c);
        UE_LOGI("scan_hub: consumer '%s' registration QUEUED (pass active)", c.name);
        return;
    }
    g_pendingRegs.push_back(c);
    AdoptPendingRegistrations();
}

void Tick() {
    if (g_inPass) {
        RunSlice();
        return;
    }
    if (steady_clock::now() < g_nextPassDue) return;
    if (StartPass()) RunSlice();
}

void ForceSyncFullPass() {
    // Parity drill mode A: one complete FULL pass inside this GT call (no slicing), so the
    // caller can compare indexes against an independent probe walk with zero staleness.
    if (g_inPass) AbortPass("ForceSyncFullPass preempt");
    for (Row& r : g_rows) r.stableScans = 0;  // force FULL
    if (!StartPass()) return;
    while (g_inPass) {
        // RunSlice re-checks validity between "slices"; budget still applies per call but we
        // loop to completion synchronously.
        if (RunSlice()) break;
        if (!g_inPass) break;  // aborted
    }
}

bool PassActive() { return g_inPass; }

}  // namespace coop::element::scan_hub
