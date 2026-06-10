// ui/hud.h -- the always-on passive coop HUD surface (nameplates + chat feed +
// the dev object-overlay labels).
//
// A presentation surface on ui::imgui_overlay, like ui::scoreboard -- but PASSIVE
// and ALWAYS-ON: it draws whenever there is something to show (a remote puppet to
// label, or a chat line), without taking the cursor or swallowing input (so the
// player keeps playing while it overlays). The overlay's interactive surfaces (F1
// menu, server browser, ...) draw ON TOP of this.
//
// Principle 7: pure presentation. The facts come from game-thread snapshots --
// coop::nameplate (projected screen-space labels) + coop::chat_feed (event lines)
// + coop::dev::object_overlay (projected world-object debug labels, dev-toggled).
// This file only draws them via ImGui.

#pragma once

namespace ui::hud {

// True if there is anything to draw (a nameplate or a chat line). The overlay
// checks this each frame to decide whether to run the always-on HUD pass. Lock-free.
bool IsActive();

// Draw the nameplates (ImGui background draw list -- always behind windows) + the
// chat feed (a borderless, input-transparent corner panel). Called inside the
// overlay's ImGui frame (render thread) when IsActive().
void Render();

}  // namespace ui::hud
