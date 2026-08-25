// coop/dev/worldless_frames.h -- how many frames does the game PRESENT while no world
// exists?
//
// RUNG 0 of the native server browser's P1 measurement (docs/MULTIPLAYER_UI.md section 8),
// split into its own TU because it shares nothing with the UMG census next door: a
// different thread (render, not game), a different call site (the Present detour, not the
// ui_menu Tick observer) and no state in common but the arming flag.
//
// WHY THE NUMBER MATTERS. A UMG surface needs a world to draw into; an ImGui Present hook
// does not. So the whole of question O4 -- "can UMG serve join_curtain / loading_screen /
// boot_warning_dialog, i.e. is the ~3,700 LOC of overlay substrate retirable" -- reduces to
// whether the game ever presents a frame with no world. Banking either answer while the
// instrument sits unused in the same loop is the blind-instrument shape that doc keeps
// catching.
//
// Armed by the SAME row as the census: `[dev] native_ui_probe=1`.

#pragma once

namespace coop::dev::worldless_frames {

// Call once per PRESENTED frame, from the overlay's Present detour. Render thread. Costs
// one relaxed bool load when disarmed.
void NoteFrame();

}  // namespace coop::dev::worldless_frames
