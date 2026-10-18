// coop/dev/camera_drill.h -- [dev] a picked-up camera: does it stick to its wall again, and by whose hand.
//   HOST   -- once the director can route (checked each second), with no wait for a client: finds a flat wall that a
//             walker reaches by a NavMesh route (retried each second, given up after 60 s of world time), hangs a
//             prop_camera_bad_C on it, sticks it through the prop-stick lane's converge, and says
//             "camera_drill: host READY stuck key=... at=..." only if it reads frozen AND attached to a wall actor.
//   CLIENT -- scans for the nearest stuck camera in its world, walks to it by the director, grabs it by the use key's
//             chain, holds 3 s of the world's own clock, lets go, and watches 2.5 s more. The first stuck sample while
//             held is "camera_drill: re-stuck while held at +...ms"; a line every 0.25 s gives the held camera's and the
//             player's distance off the wall plane.
// Arms: grab (the walk ends 70 cm off the wall), grab_near (as close as the director allows) and host_grab_near (the
// host does the near grab on its own camera, the control; no client is needed). The deciding read is the prop-stick
// lane's own lines. "[CAMERA-DRILL] FAIL" is the lane failing a step (--fail-marker), "[CAMERA-DRILL] ABANDONED" the
// drill unable to do its part (--dead-marker), and its DONE line (--done-marker) is the grabber's, client or host.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::camera_drill {

// Game thread, once per pump tick; a latched bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client runs its legs again; the host's camera stays, or is hung again if it is gone.
void OnDisconnect();

}  // namespace coop::dev::camera_drill
