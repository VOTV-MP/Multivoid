// ui/config_review_panel.h -- the T10 config review panel (render thread).
//
// Shows the boot sweep's findings (rejected values, unknown keys, differing
// duplicates, identity durability) on the ungated overlay root, so it draws
// at the main menu before any session exists (design T10; the HUD root is the
// one measured-ungated surface, F42). Persistent until dismissed; dismissal
// is session-local -- a launch with findings shows them again. Owner actions
// (keep line N / tidy up the file) route through coop::config_review.

#pragma once

namespace ui::config_review_panel {

// True while the sweep has rows and the user hasn't dismissed them this
// session. Gates Render() and joins the overlay's AnyOpen/CaptureActive.
bool IsOpen();

void Render();

}  // namespace ui::config_review_panel
