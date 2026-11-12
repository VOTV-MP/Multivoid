// ue_wrap/devices/video_camera.h -- what the video camera's overlay shows on this machine. The video cameras
// (prop_vidcam_C and its five qualities) have no state of their own: each world has one camera manager
// (RCM_cameraManager_C, the gamemode's `RCM_manager`) that records through whichever camera began play last. Its
// overlay widget (`camUI`, a ui_vcam_C) is shown while the player holds a video camera or a recording runs, the
// dropped camera's included. A recording counts as a running event (lib_C::setEvent) until it stops, and only the
// camera's use with Alt held stops it (a click or a plain use pauses it), or its 720 s limit, or a flat battery.
// Engine-wrapper layer: reads only, no coop state. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::video_camera {

enum class View : uint8_t {
    None,         // no overlay: no camera in hand and nothing recording, or no world
    Viewfinder,   // the overlay, a camera in hand, not recording
    Recording,    // a recording runs (or is paused), held or dropped
};

// The overlay's state now. False when the gamemode, its manager or the overlay widget does not read.
bool ReadView(View& out);

}  // namespace ue_wrap::video_camera
