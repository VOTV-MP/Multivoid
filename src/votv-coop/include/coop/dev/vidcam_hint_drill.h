// coop/dev/vidcam_hint_drill.h -- drill: the HUD's video camera hint follows the camera in the hand (config row
// vidcam_hint_drill = off|host|client, env VOTVCOOP_VIDCAM_HINT_DRILL; the named role only).
//
// Once per process, when the peer is in a gameplay world with its player, it puts a video camera into the local hand
// through the game's own hold step (coop/dev/drill_hand) and waits until the game's own overlay (the widget's
// visibility the camera manager's updUI wrote) shows the viewfinder; the state the hint reads
// (coop/player/vidcam_hint::Current, published at that updUI) must then say so too, or the verdict is FAIL at once.
// It empties the hand and decides the same way when the overlay is gone. `rig: VERDICT vidcam-hint PASS` when both
// agreed; INVALID when the hand could not be filled or the game never changed its overlay within a bound. The red
// (vidcam_hint_red) leaves the watch unregistered, so the state never moves and the verdict must be FAIL.
// Lines tagged [VIDCAM-HINT-DRILL].

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::vidcam_hint_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: a wait in progress is dropped. The once-per-process latch is not cleared.
void OnDisconnect();

}  // namespace coop::dev::vidcam_hint_drill
