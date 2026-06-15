// ui/voice_icons.cpp -- see ui/voice_icons.h.

#include "ui/voice_icons.h"

#include <cmath>

namespace ui::voice_icons {

namespace {

using coop::voice_chat::VoiceIcon;

// All shapes are built in a unit space where the icon is `h` px tall and
// centered at `c`. Stroke widths scale with h so the glyphs stay readable
// from nameplate (10 px) to panel (18 px) sizes.

void DrawMic(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float t) {
    // Capsule body.
    const float bw = h * 0.30f, bh = h * 0.52f;
    const ImVec2 bmin(c.x - bw * 0.5f, c.y - h * 0.46f);
    const ImVec2 bmax(c.x + bw * 0.5f, bmin.y + bh);
    dl->AddRectFilled(bmin, bmax, col, bw * 0.5f);
    // Stand arc (open up), stem, base.
    const float ar = h * 0.30f;
    const ImVec2 ac(c.x, bmax.y - h * 0.10f);
    dl->PathArcTo(ac, ar, 3.14159265f * 0.15f, 3.14159265f * 0.85f, 12);
    dl->PathStroke(col, 0, t);
    dl->AddLine(ImVec2(c.x, ac.y + ar), ImVec2(c.x, c.y + h * 0.42f), col, t);
    dl->AddLine(ImVec2(c.x - bw * 0.6f, c.y + h * 0.46f),
                ImVec2(c.x + bw * 0.6f, c.y + h * 0.46f), col, t);
}

void DrawSpeaker(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    // Box + cone wedge.
    const float bx = h * 0.16f;
    dl->AddRectFilled(ImVec2(c.x - h * 0.38f, c.y - bx), ImVec2(c.x - h * 0.38f + bx, c.y + bx),
                      col);
    const ImVec2 tri[3] = {ImVec2(c.x - h * 0.10f, c.y - h * 0.34f),
                           ImVec2(c.x - h * 0.10f, c.y + h * 0.34f),
                           ImVec2(c.x - h * 0.38f, c.y)};
    dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);
}

void DrawWaves(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float t, int n) {
    for (int i = 0; i < n; ++i) {
        const float r = h * (0.18f + 0.16f * static_cast<float>(i + 1));
        dl->PathArcTo(ImVec2(c.x - h * 0.06f, c.y), r, -3.14159265f * 0.28f,
                      3.14159265f * 0.28f, 10);
        dl->PathStroke(col, 0, t);
    }
}

void DrawSlash(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float t) {
    dl->AddLine(ImVec2(c.x - h * 0.46f, c.y + h * 0.46f),
                ImVec2(c.x + h * 0.46f, c.y - h * 0.46f), col, t * 1.3f);
}

}  // namespace

void Draw(ImDrawList* dl, ImVec2 c, float size, VoiceIcon icon, float alpha) {
    if (icon == VoiceIcon::None || alpha <= 0.02f) return;
    const float h = size;
    const float t = (h * 0.10f < 1.0f) ? 1.0f : h * 0.10f;
    const int a = static_cast<int>(alpha * 255.0f);
    const ImU32 white = IM_COL32(235, 240, 245, a);
    const ImU32 green = IM_COL32(120, 235, 140, a);
    const ImU32 red = IM_COL32(245, 105, 95, a);
    const ImU32 grey = IM_COL32(150, 155, 162, a);

    switch (icon) {
    case VoiceIcon::Talking:
        DrawMic(dl, c, h, green, t);
        break;
    case VoiceIcon::Whispering:
        DrawMic(dl, ImVec2(c.x - h * 0.14f, c.y), h * 0.92f, green, t);
        DrawWaves(dl, ImVec2(c.x + h * 0.26f, c.y), h * 0.8f, green, t, 2);
        break;
    case VoiceIcon::MicMuted:
        DrawMic(dl, c, h, grey, t);
        DrawSlash(dl, c, h, red, t);
        break;
    case VoiceIcon::Disabled:
        DrawSpeaker(dl, c, h, grey);
        DrawSlash(dl, c, h, red, t);
        break;
    case VoiceIcon::Disconnected: {
        // "No signal": outlined circle + slash (reads at 10 px where a
        // broken-link glyph would smear).
        dl->AddCircle(c, h * 0.42f, grey, 0, t);
        DrawSlash(dl, c, h * 0.84f, grey, t);
        break;
    }
    default:
        break;
    }
}

const char* Label(VoiceIcon icon) {
    switch (icon) {
    case VoiceIcon::Talking: return "Talking";
    case VoiceIcon::Whispering: return "Whispering";
    case VoiceIcon::MicMuted: return "Mic muted";
    case VoiceIcon::Disabled: return "Voice off";
    case VoiceIcon::Disconnected: return "Voice disconnected";
    default: return "";
    }
}

}  // namespace ui::voice_icons
