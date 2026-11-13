// coop/player/vidcam_hint.h -- the video camera's overlay, published for the HUD's hint line. A dropped camera keeps
// recording, and while it records the game counts an event as running, so the pause menu's Save stays off; the game
// says nothing of how a recording stops (Alt with the camera's use; a click or a plain use only pauses it). The HUD
// says it while the overlay shows (ui/hud). The overlay's state is read on the game thread when the game changes it
// (a watch on the camera manager's updUI, the one function that writes the overlay's visibility) and published; the
// render thread reads the published state.

#pragma once

#include "ue_wrap/devices/video_camera.h"

namespace coop::vidcam_hint {

// Register the watch on the manager's updUI once the script gate stands, and read the overlay once then.
// Idempotent; safe every frame. Game thread, the frame tail.
void Install();

// The last published state, None once the world it was read in has gone. Any thread.
ue_wrap::video_camera::View Current();

}  // namespace coop::vidcam_hint
