// coop/dev/vidcam_hint_drill.h -- drill: the HUD's video camera hint follows the camera in the hand (config row
// vidcam_hint_drill = off|host|client, env VOTVCOOP_VIDCAM_HINT_DRILL; the named role only).
//
// Once per process, when the peer is in a gameplay world with its player, it puts a video camera into the local hand
// through the game's own hold step (coop/dev/drill_hand), waits until the overlay state the hint reads
// (coop/player/vidcam_hint::Current, published at the camera manager's updUI) says the viewfinder shows, empties the
// hand, and waits until it says nothing shows. `rig: VERDICT vidcam-hint PASS` when both held; FAIL when one did not
// within its bound; INVALID when the hand could not be filled. The red (vidcam_hint_red) leaves the watch
// unregistered, so the viewfinder never shows and the verdict must be FAIL. Lines tagged [VIDCAM-HINT-DRILL].

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::vidcam_hint_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: a wait in progress is dropped. The once-per-process latch is not cleared.
void OnDisconnect();

}  // namespace coop::dev::vidcam_hint_drill
