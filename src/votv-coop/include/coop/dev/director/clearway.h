// coop/dev/director/clearway.h -- the board between the walk's Goto and its clearway process (proc_clearway.cpp). A
// walker stopped by a physics prop on its route's leg does what a player does at a doorway full of boxes: it grabs the
// prop, turns to the freer side of the route's leg, walks a short way with it, and lets go. Goto offers the prop it
// named; the process drags it aside with the game's own physics grab and let-go, a bounded number of times per walk.
// A prop carried before is carried again only when it blocks again and has moved since its last carry began (a pile,
// or a prop that slid back); one that did not move is not. A prop the game's grab does not take stays the detour's.
// Dev director only; game thread.

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
    ue_wrap::FVector heading{};         // the waypoint Goto was heading for: the prop is carried across the line to it
    int carries = 0;                    // props dragged aside so far
    int detoured = 0;                   // props the grab did not take, left to the detour
    uint32_t attempts = 0;              // moves when an attempt ends, carried or not: Goto re-paths from where it stands

    // Goto's side. True when `prop` is offered to the process: armed, under the bound, not tried before (or tried and
    // moved since its last carry began), and not a child actor (the grab itself decides the rest). False, with the
    // reason logged, leaves the prop to the detour.
    bool Offer(void* prop, const ue_wrap::FVector& headingTo);

    // The process's side: a prop taken up is recorded where it stands, and is offered again only once it has moved. A
    // `refused` prop (a child actor) is never offered again. A tried prop that died is forgotten.
    void MarkTried(void* prop, bool refused = false);

private:
    struct TriedProp {
        ue_wrap::CachedObjRef ref;
        ue_wrap::FVector at{};          // where it stood when its last carry began
        bool refused = false;
    };
    TriedProp* FindTried(void* prop);
    std::vector<TriedProp> tried_;
};

}  // namespace coop::director
