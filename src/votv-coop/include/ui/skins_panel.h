// ui/skins_panel.h -- the F1 Cosmetics > Skins model browser (gmod-style tiles).
//
// Presentation only (principle 7): renders the skin catalog
// (coop::skins::Entries) as preview tiles, highlights the current choice
// (coop::local_body::LocalSkinNameCopy) and forwards a click to
// coop::local_body::RequestSkin (which posts the game-thread apply + the wire
// announce). Preview textures are lazily created via
// ui::imgui_overlay::CreateTextureFromImageFile and cached here; the Refresh
// button rescans the pak folder + drops the texture cache. Render thread.

#pragma once

namespace ui::skins_panel {

// One content-pane item inside the F1 menu (dev_menu Cosmetics > Skins).
void Render();

}  // namespace ui::skins_panel
