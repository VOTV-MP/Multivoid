// ui/world_rules_panel.h -- the F1 > World > Rules content pane (EVERYONE).
//
// A read-only list of the world rules THIS peer is running under
// (mainGameInstance.gameRules -- fall damage, difficulty, seasons, funny,
// custom content, food spoilage, the minigame toggles, ...) plus the gamemode.
// Shown to host, clients, AND solo (a non-dev, non-host F1 category), per the
// user ask 2026-07-08: "F1 menu section showing the server/world rules FOR
// EVERYONE."
//
// It reads the LOCAL GameInstance copy on purpose: a joining client boots from
// the host's live-captured save, so (once VOTV's load applies localGameRules ->
// GI.gameRules) every peer's list equals the host's -- and if it DOESN'T, the
// mismatch stays visible here instead of being masked by a broadcast. The read
// is a one-shot game-thread snapshot taken on (re)open; the render just paints
// the cached result. Principle 7: the reflected read lives in
// ue_wrap::game_rules; this file only renders.

#pragma once

namespace ui::world_rules_panel {

// Draw the World Rules pane content (called from dev_menu's content child).
void Render();

}  // namespace ui::world_rules_panel
