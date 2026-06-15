// ui/voice_icons.h -- vector-drawn voice state icons (mic / speaker / slash /
// whisper) shared by the scoreboard column, the nameplate badge and the local
// HUD indicator. Drawn as ImGui draw-list primitives (the port design SS3.1:
// we ship no image assets and SVC's PNGs carry its license); white-tinted,
// alpha-multiplied by the caller's surface alpha. Render thread only.

#pragma once

#include "coop/voice/voice_chat.h"

#include "imgui.h"

namespace ui::voice_icons {

// Draw `icon` centered at `c`, `size` px tall, alpha 0..1. None draws nothing.
void Draw(ImDrawList* dl, ImVec2 c, float size, coop::voice_chat::VoiceIcon icon,
          float alpha);

// One-line state label for tooltips ("Talking", "Mic muted", ...).
const char* Label(coop::voice_chat::VoiceIcon icon);

}  // namespace ui::voice_icons
