// coop/dev/worldless_frames.h -- how many frames does the game PRESENT while no
// world exists?
//
// The frame half of the native browser's pre-build measurement, split into its own
// TU because it shares nothing with the UMG census next door: a different thread
// (render, not game), a different call site (the Present detour, not the ui_menu
// Tick observer), and no state in common but the arming flag.
//
// WHY THE NUMBER MATTERS. A UMG surface needs a world to draw into; an ImGui
// Present hook does not. So the question of whether UMG could serve join_curtain,
// loading_screen and boot_warning_dialog -- that is, whether the ~3,700 LOC of
// overlay substrate is retirable -- reduces to whether the game ever presents a
// frame with no world.
//
// Armed by the SAME row as the census: `[dev] native_ui_probe=1`.

#pragma once

namespace coop::dev::worldless_frames {

// Call once per PRESENTED frame, from the overlay's Present detour. Render thread. Costs
// one relaxed bool load when disarmed.
void NoteFrame();

}  // namespace coop::dev::worldless_frames
