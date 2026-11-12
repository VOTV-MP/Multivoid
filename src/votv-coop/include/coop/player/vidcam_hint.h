// coop/player/vidcam_hint.h -- the video camera's overlay, published for the HUD's hint line. A dropped camera keeps
// recording, and while it records the game counts an event as running, so the pause menu's Save stays off; the game
// says nothing of how a recording stops (Alt with the camera's use; a click or a plain use only pauses it). The HUD
// says it while the overlay shows (ui/hud). Read from ue_wrap/devices/video_camera on the game thread at 4 Hz; the
// render thread reads the published state.

#pragma once

#include "ue_wrap/devices/video_camera.h"

namespace coop::vidcam_hint {

// Sample the overlay (at most every 250 ms). Game thread, the frame tail.
void Update();

// The last sample. Any thread.
ue_wrap::video_camera::View Current();

}  // namespace coop::vidcam_hint
