// ue_wrap/settled_object_scan.h -- the STREAM-SETTLE scan discipline over IncrementalObjectScan.
//
// WHY (2026-07-04, the 18:41 keypad-0-sync root): a raw NextRange tail-scan is only correct while
// its index's world is ALIVE. At a world reload (host session start: menu world -> save world) the
// periodic tail scan PRUNES the dead old-world actors (index -> 0) while the new world's actors land
// in GC-RECYCLED GUObjectArray slots BELOW `scannedTo` -- the tail never sees them and the index
// stays empty until the rare full backstop. Log-proven 18:41:10: every raw-NextRange index (keypad/
// power/window/grime) rebuilt to 0 while every settle-gated interactable_channel index (door/light/
// container/appliance) rebuilt correctly through the same reload.
//
// THE DISCIPLINE (extracted verbatim from interactable_channel.h's proven L5 take-3 logic; that
// channel now consumes THIS component -- one implementation, RULE 2):
//   - FULL-walk while the live count is still CHANGING (streaming in, OR a world reload just pruned
//     the index -- any count move re-arms full walks; a full walk cannot miss a recycled-slot actor).
//   - Switch to the cheap TAIL-scan only after `settleScans` consecutive scans with an UNCHANGED,
//     NON-ZERO live count (~world done streaming this class).
//   - Under tail-scan, a FULL re-scan every `backstopFullEvery` calls (staggered per consumer so the
//     channels' backstops never coincide) recovers gap-streamed / slot-reused stragglers.
//   - An index that goes EMPTY (or was never populated) keeps full-walking -- pre-load 0 cannot
//     settle, and a reload's prune-to-0 immediately re-arms the full walk (the 18:41 cure).
//
// Usage (the shape every *_sync RebuildIndex uses):
//   static ue_wrap::scan::SettledObjectScan sScan;          // or a member; auto-staggered
//   const ue_wrap::scan::ScanRange r = sScan.Begin();
//   ... scan [r.begin, r.end); if (r.isFull) clear index first; else prune dead after ...
//   sScan.End(indexSizeNow);                             // feeds the settle gate
//
// Game-thread only (NumObjects + the state are GT-serial, like every *_sync Tick).
#pragma once

#include "ue_wrap/core/incremental_object_scan.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::scan {

// Proven tuning (interactable_channel L5 take-3, 2026-06-23): at a 2s caller throttle,
// settle after 15 unchanged scans (~30s -- settling earlier is the take-1 door 57->19
// regression) and full-backstop every 30 ticks (~60s -- 150/~5min never fired in short runs).
inline constexpr int kSettleScansDefault   = 15;
inline constexpr int kBackstopEveryDefault = 30;

struct SettledObjectScan {
    explicit SettledObjectScan(int settleScans = kSettleScansDefault,
                               int backstopFullEvery = kBackstopEveryDefault)
        : settleScans_(settleScans), backstopFullEvery_(backstopFullEvery) {
        // Coprime stride over the backstop period -> consumers' full-rescans de-correlate
        // (the channel ctor's sStaggerSeq pattern, now owned here for every consumer).
        static int sStaggerSeq = 0;
        staggerOffset_ = (sStaggerSeq++ * 7) % (backstopFullEvery_ > 0 ? backstopFullEvery_ : 1);
        scan_.sinceFull = staggerOffset_;
    }

    bool settled() const { return stableScans_ >= settleScans_; }

    // The range to scan this call. NOT-settled -> FULL walk (and the tail cursor is parked at the
    // live end so the first post-settle tail starts clean, with the backstop counter staggered).
    ScanRange Begin() {
        if (!settled()) {
            ScanRange r{0, ue_wrap::reflection::NumObjects(), true};
            scan_.scannedTo = r.end;
            scan_.sinceFull = staggerOffset_;
            return r;
        }
        return NextRange(scan_, backstopFullEvery_);
    }

    // Feed the post-scan index size. Any count CHANGE (grow = streaming, shrink = deaths/reload
    // prune) resets the settle counter -> full walks resume next call. A zero count NEVER settles
    // -- deliberately (perf-audit 2026-07-04 W-a weighed): letting an empty index settle (e.g. in
    // the MENU world) would blind it through the next world load when every actor lands in a
    // GC-recycled slot below the tail cursor (zero appends -> zero count change -> nothing until
    // the backstop) -- the EXACT 18:41 keypad outage this component exists to kill. The cost of
    // never-settling is a per-2s full walk ONLY while a class is genuinely absent from the world;
    // every class this project indexes is populated on the main map, so the waste is hypothetical
    // while the outage was real. Do not "optimize" this without re-reading the 18:41 post-mortem
    // ([[project-test-1841-keypad-index-pile-type-2026-07-04]]).
    void End(size_t liveCount) {
        if (liveCount > 0 && liveCount == lastCount_) {
            if (stableScans_ < settleScans_) ++stableScans_;
        } else if (liveCount != lastCount_) {
            stableScans_ = 0;
        }
        lastCount_ = liveCount;
    }

private:
    int settleScans_;
    int backstopFullEvery_;
    int staggerOffset_ = 0;
    int stableScans_ = 0;
    size_t lastCount_ = static_cast<size_t>(-1);  // sentinel: the first scan always counts as changed
    IncrementalObjectScan scan_;
};

}  // namespace ue_wrap::scan
