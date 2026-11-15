// coop/dev/event_fire_drill.h -- [dev] the host fires one named event once a client stands in its
// world, and the run then stays up for a set time so both logs record what each lane did with it.
//
// `event_fire_drill` is `<name>[:<seconds>[:<class>]]` (a class moves the host player to its first actor first): a name of the dev event table (coop/dev/event_trigger,
// the F1 menu's list: an eventer row, a prank special or an ambient verb), fired through the same
// dispatch the menu uses, and how long to watch after it (60 s when not given). The host logs
// `[EVENT-FIRE-DRILL] [H] fired '<name>' (1|0)` and, at the end of the watch, `DONE`. There is no
// verdict of its own: the lanes' lines on both peers are the evidence.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::event_fire_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::event_fire_drill
