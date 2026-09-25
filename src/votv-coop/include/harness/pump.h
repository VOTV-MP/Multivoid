// harness/pump.h -- the ONE task this harness posts to the game thread per tick, and the
// watchdogs that deliberately do not ride inside it. Every TimelineThread loop that must keep
// the mod alive -- the play loop and the world-boot waits -- drives the same calls from here,
// so a wait outside gameplay is never a loop that forgot half of them.

#pragma once

#include "ue_wrap/core/game_thread.h"

#include <utility>

namespace coop::net { class Session; }

namespace harness::pump {

// The session every tick below drives, handed over once at process boot. Nothing here works
// before it, and each call is a no-op until it arrives.
void SetSession(coop::net::Session* session);

// The coalescing itself, split out so the template below stays a thin header body.
bool BeginComposite();
void EndComposite();
bool CompositeDrainIsNew();

// Post `body` as this tick's composite. The TimelineThread posts at 60 Hz and the game thread
// runs posted tasks only at an outermost dispatch, so one script body -- a blocking world
// load's -- holds every task for its whole length. A composite is posted only while none is
// queued, and runs its body at most once in a drain: a drain runs every task posted while it
// runs, so a composite posted as a long tick returned would otherwise run a second tick straight
// after the first. A stall of any length therefore ends in one session tick, never a run of ticks
// back to back with no engine frame between them, each advancing every tick-counted window. The
// flag clears as the composite returns, a faulting body included (the image unwinds destructors
// on a structured exception), so one faulted composite cannot stop the next. Composites are
// idempotent per-tick logic, so a skipped post or body is not lost work.
//
// A TEMPLATE on purpose: a caller's composite closure is a couple of bytes and rides inside
// std::function's small-object buffer, while taking it as a std::function here would wrap one
// inside another and heap-allocate on every post, sixty times a second.
template <typename Body>
void PostComposite(Body&& body) {
    if (!BeginComposite()) return;
    ue_wrap::game_thread::Post([body = std::forward<Body>(body)] {
        struct Clear { ~Clear() { EndComposite(); } } clear;
        if (!CompositeDrainIsNew()) return;
        body();
    });
}

// The composite every wait loop OUTSIDE gameplay posts: the session tick the save transfer lives
// in, then the frame tail. The loop that waits for the transfer is the loop that posts the tick the
// transfer runs in, so without this a menu-mode join deadlocks at "Connecting".
void PostMenuTick();

// The tail every composite runs, in a session or out of one, at the menu or in a world: the
// nameplates, the dev overlays, the chat feed, the lookup parity probe and the shutdown hooks. One
// list, so an item added for one composite runs on the other's frames too: a join's menu is ticked by
// this module's own composite, not the play loop's. Game thread.
void TickFrameTail();

// The watchdogs that cover a failure of the PUMP itself, and therefore the one thing that must
// not ride in the pump's own composite: a stalled game thread stops the watchdog and the task it
// is supposed to be watching together. Called from the thread that POSTS the composite instead --
// safe there, since it touches atomics and posts its flee.
void TickWatchdogs();

}  // namespace harness::pump
