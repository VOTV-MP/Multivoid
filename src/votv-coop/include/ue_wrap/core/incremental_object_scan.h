// ue_wrap/incremental_object_scan.h -- INCREMENTAL GUObjectArray discovery (L5 FPS root fix, 2026-06-23).
//
// WHY: a dozen *_sync subsystems each rebuilt their interactable index by walking ALL ~237k UObjects every
// 2s. Measured cost: 5-20ms per walk (game thread) firing ~every 2.4s in a live session = the periodic FPS
// stutter. A high-water gate (skip when NumObjects is flat) was NOT enough: any spawn changes NumObjects, so
// in active play the gate fires constantly and the full walks still run + cluster. The ROOT fix is to stop
// full-walking: scan only the NEW objects (the array TAIL [scannedTo, NumObjects())) each call -- O(new) ~
// microseconds -- and prune the small index, with a RARE full re-scan (~5 min) as the slot-reuse backstop.
//
// SLOT-REUSE caveat: UE recycles freed GUObjectArray slots, so a runtime-spawned actor can land BELOW the
// tail (NumObjects flat) and be missed by a pure tail-scan. STATIC-actor subsystems (window/keypad/power/
// turbine -- level-loaded, rarely created at runtime) tolerate this: the ~5 min full safety catches the rare
// drift. DYNAMIC-actor subsystems (a re-piled chipPile reuses the freed clump slot frequently) must instead
// register at the spawn SEAM (the convert hook) -- NOT rely on the tail-scan -- so they do not use this for
// their dynamic births (they register at their spawn seam instead). This supersedes the earlier
// per-spawn-frequency gate: gating a 5-20ms full walk only made it rarer, not cheap -- this makes it cheap.
//
// Usage:
//   size_t RebuildIndex() {
//     static ue_wrap::scan::IncrementalObjectScan sScan;
//     const auto r = ue_wrap::scan::NextRange(sScan);   // {begin,end,isFull}
//     // scan [r.begin, r.end): add matches to the index;  if (r.isFull) clear first;  else prune dead after.
//   }
// Game-thread only (NumObjects + the static state are GT-serial, like every *_sync Tick).
#pragma once

#include "ue_wrap/core/reflection.h"  // NumObjects

#include <cstdint>

namespace ue_wrap::scan {

struct IncrementalObjectScan {
    int32_t scannedTo = 0;  // GUObjectArray count covered so far (the tail starts here)
    int     sinceFull = 0;  // throttle-ticks since the last FULL re-scan (the safety counter)
};

struct ScanRange {
    int32_t begin;   // first index to scan
    int32_t end;     // one past the last (== NumObjects() now)
    bool    isFull;  // true = a full re-scan (caller clears its index first); false = tail-only (caller prunes after)
};

// Returns the index range to scan this call. Normally the TAIL [scannedTo, NumObjects()) (only new objects).
// Every `fullEvery` calls -- OR if the array SHRANK (n < scannedTo, e.g. a world travel freed objects below
// the tail) -- returns a FULL [0, NumObjects()) re-scan (the slot-reuse backstop). `fullEvery` is in throttle-
// ticks of the CALLER; at a 2s throttle, 150 = ~5 min (a single full walk every 5 min = negligible).
inline ScanRange NextRange(IncrementalObjectScan& s, int fullEvery = 150) {
    const int32_t n = ue_wrap::reflection::NumObjects();
    if (++s.sinceFull >= fullEvery || n < s.scannedTo) {
        s.sinceFull = 0;
        s.scannedTo = n;
        return ScanRange{0, n, true};
    }
    const int32_t begin = s.scannedTo;
    s.scannedTo = n;
    return ScanRange{begin, n, false};
}

}  // namespace ue_wrap::scan
