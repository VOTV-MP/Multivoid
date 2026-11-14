// ui/build_update_dialog.h -- the prompt of a host's build offer (coop/net/build_share): the question
// with what accepting means, the download's progress, and how it ended. Drawn by the overlay as a
// modal over whatever the join left on screen. Render thread.

#pragma once

namespace ui::build_update_dialog {

// An offer waits for the player, a download runs, or an outcome has not been acknowledged.
bool IsOpen();

void Render();

}  // namespace ui::build_update_dialog
