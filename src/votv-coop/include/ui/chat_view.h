// ui/chat_view.h -- the chat surface: the passive feed AND the T-activated history
// reveal (render thread, ImGui). Extracted from ui/hud.cpp 2026-07-29 when the
// reveal + scroll gave the chat block its own state; hud.cpp keeps the stateless
// passive overlays (nameplates, object/bone ESP, the voice badge).
//
// TWO THINGS ARE DRAWN FROM ONE STORE. While chat is closed this is exactly the
// feed it always was: up to six live lines fading on their TTL. Pressing T ramps in
// the RETAINED tier as well -- the lobby's history -- and every row goes opaque for
// as long as the surface is up. The ramp lives HERE, on the render side, and the
// store's published alpha stays the pure TTL curve: folding the reveal into the
// published value would make a retained row's alpha jump 0 -> 1, which is precisely
// the "can't happen" condition the store's resurrection probe watches for, and that
// probe's silence is the evidence the 2026-07-09 flicker fix rests on.
//
// New messages ALWAYS arrive and move the view while you read (user's rule). The one
// exception is a reader who has deliberately paged back with PgUp/PgDn; that pins
// the view until they page forward past the newest line, or close chat.

#pragma once

namespace ui::chat_view {

// Draw the feed / reveal. Call from the per-frame ImGui pass, inside the frame
// (it reads ImGui key state) and before the interactive surfaces so they layer on
// top. No-ops when there is nothing to show.
void Draw();

}  // namespace ui::chat_view
