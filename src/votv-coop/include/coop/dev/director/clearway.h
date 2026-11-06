// coop/dev/director/clearway.h -- the board between the walk's Goto and its clearway process (proc_clearway.cpp). A
// walker stopped by a physics prop on its route's leg does what a player does at a doorway full of boxes: it picks the
// prop up, turns away from the way on, and sets it down. Goto offers the prop it named; the process carries it aside
// with the game's own grab and drop, a bounded number of times per walk, each prop once. A prop the hold refuses stays
// the detour's. Dev director only; game thread.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <vector>

namespace coop::director {

constexpr int kMaxCarries = 6;   // props carried aside per walk: a pile of boxes, not a warehouse

struct ClearwayBoard {
    bool armed = false;                 // the walk's process set has a clearway process, and the dev row leaves it on
    ue_wrap::CachedObjRef request;      // the prop Goto offered, until the process takes it up
    ue_wrap::FVector heading{};         // the waypoint Goto was heading for: the carried prop is set down away from it
    int carries = 0;                    // props carried aside so far
    uint32_t attempts = 0;              // moves when an attempt ends, carried or not: Goto re-paths from where it stands

    // Goto's side. True when `prop` is offered to the process: armed, under the bound, not tried before, and a prop
    // the game would let a hand hold (its own flags: not heavy, not refused, not a child actor). False, with the
    // reason logged, leaves the prop to the detour.
    bool Offer(void* prop, const ue_wrap::FVector& headingTo);

    // The process's side: a prop, once taken up, is never offered again. A tried prop that died is forgotten.
    void MarkTried(void* prop);

private:
    bool Tried(void* prop);
    std::vector<ue_wrap::CachedObjRef> tried_;
};

}  // namespace coop::director
