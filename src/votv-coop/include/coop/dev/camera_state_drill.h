// coop/dev/camera_state_drill.h -- [dev] a security camera's two bits are the host's on every peer: the camera
// drill's acceptance arms (camera_drill = host-toggle | client-toggle | light | host-event | join).
//   host-toggle   -- the host spawns a camera and switches it on with its own use; the client reads `active` on.
//   client-toggle -- the client presses the camera's use: its own copy must not move (the press is refused and
//                    sent), and it reads `active` on once the host has performed it.
//   light         -- the client walks to the base laptop, selects the camera in the laptop's panel and clicks the
//                    camera-light button: the same refusal, then it reads `isLight` on.
//   host-event    -- the host puts a camera beside the gray event's controller, switches it on and fires the event
//                    (graysforest), switching the camera on again whenever the event has put it off; the client
//                    must read it on and then off, the host having only ever switched it on.
//   join          -- the host switches a camera's `active` and `isLight` on before any client joins; every client
//                    session reads both on once its world is ready, a rejoin's joiner included.
// Every arm ends on the client's [CAMERA-DRILL] line, PASS or FAIL after the arm's name, from the client's read of
// the host-written bit; a line with ABANDONED is the drill unable to do its part. The host says READY once its
// camera stands and the lane indexes it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::camera_state_drill {

// Game thread, once per pump tick; a latched string compare when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client runs its legs again; the host's camera stays.
void OnDisconnect();

}  // namespace coop::dev::camera_state_drill
