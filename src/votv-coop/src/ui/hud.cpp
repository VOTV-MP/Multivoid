// ui/hud.cpp -- see ui/hud.h.

#include "ui/hud.h"

#include "coop/social/chat_feed.h"
#include "coop/dev/object_overlay.h"
#include "coop/nameplate.h"
#include "coop/voice/voice_chat.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace ui::hud {
namespace {

// Nameplate BASE text size (px) -- the size up CLOSE. The whole plate scales DOWN with
// distance (p.scale, ~1/distance, computed in coop::nameplate::DistanceScale) so it stays
// proportional to the peer's on-screen body instead of looming over a far, shrunken
// character (user 2026-06-08: the old fixed size "grew" relative to a receding peer).
// 16 px up close reads at a few metres without dominating when a peer stands right next
// to you (user 2026-06-08: 22 px felt huge up close). Distance also fades OPACITY
// (DistanceAlpha); p.scale is capped at 1.0 so the plate is never bigger than this.
constexpr float kNickPx = 16.f;

// Draw `text` at `size` with a cheap 1px outline (4-way offset) so it stays legible
// over any scene.
void TextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                  ImU32 col, ImU32 outline, const char* text) {
    dl->AddText(font, size, ImVec2(pos.x - 1.f, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x + 1.f, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y - 1.f), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y + 1.f), outline, text);
    dl->AddText(font, size, pos, col, text);
}

void DrawNameplate(ImDrawList* dl, const coop::nameplate::Plate& p) {
    char line[64];
    if (p.ping > 0)       std::snprintf(line, sizeof(line), "%s (%dms)", p.nick, p.ping);
    else if (p.ping == 0) std::snprintf(line, sizeof(line), "%s (<1ms)", p.nick);  // sub-ms LAN
    else                  std::snprintf(line, sizeof(line), "%s", p.nick);          // -1 = unmeasured

    const float a = std::clamp(p.alpha, 0.f, 1.f);
    const ImU32 white   = IM_COL32(255, 255, 255, static_cast<int>(a * 245.f));
    const ImU32 red     = IM_COL32(255, 48, 48, static_cast<int>(a * 255.f));
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 215.f));
    const ImU32 textCol = p.flash ? red : white;

    // Distance SIZE scale: the whole plate (text + bar + box + gaps) scales as ONE unit
    // so a far peer's label stays proportional to their shrunken on-screen body. p.scale
    // is 1.0 up close (capped) and shrinks ~1/distance to a legible floor far away.
    const float s = std::clamp(p.scale, 0.20f, 1.f);
    const float px = kNickPx * s;
    const float barW = 44.f * s, barH = 5.f * s;
    const float gap = 10.f * s;    // nick sits sz.y + gap above the head anchor
    const float barGap = 4.f * s;  // bar sits barGap below the anchor

    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.f, line);
    // Keep the whole nameplate on-screen: a peer in FRONT of you but whose head sits
    // past a screen edge still shows the label at the edge instead of vanishing.
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float m = 6.f;
    const float halfW = std::max(sz.x, barW) * 0.5f;
    const float ax = std::clamp(p.x, m + halfW, io.DisplaySize.x - m - halfW);
    const float ay = std::clamp(p.y, m + sz.y + gap, io.DisplaySize.y - m - barH - barGap);
    const ImVec2 textPos(ax - sz.x * 0.5f, ay - sz.y - gap);
    const ImVec2 bp(ax - barW * 0.5f, ay - barGap);

    // Readability backing: a translucent rounded box behind the nick + bar so the
    // label reads over ANY scene (bright sky, white wall, foliage).
    const float padX = 6.f * s, padY = 3.f * s;
    const ImVec2 boxMin(std::min(textPos.x, bp.x) - padX, textPos.y - padY);
    const ImVec2 boxMax(std::max(textPos.x + sz.x, bp.x + barW) + padX, bp.y + barH + padY);
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, static_cast<int>(a * 140.f)), 4.f * s);

    TextOutlined(dl, font, px, textPos, textCol, outline, line);

    // Health bar (dark red).
    const float frac = std::clamp(p.healthPct / 100.f, 0.f, 1.f);
    dl->AddRectFilled(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 160.f)));
    const ImU32 fillCol = p.flash ? red : IM_COL32(190, 30, 30, static_cast<int>(a * 235.f));
    dl->AddRectFilled(bp, ImVec2(bp.x + barW * frac, bp.y + barH), fillCol);
    dl->AddRect(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 200.f)));

    // Voice badge (v66): right of the plate backing, scaled+faded with it
    // (the SVC nameplate icon placement, design SS3.1).
    if (p.voiceIcon != 0) {
        const float ih = 13.f * s;
        ui::voice_icons::Draw(dl, ImVec2(boxMax.x + 4.f * s + ih * 0.5f,
                                         (boxMin.y + boxMax.y) * 0.5f),
                              ih, static_cast<coop::voice_chat::VoiceIcon>(p.voiceIcon), a);
    }
}

void DrawNameplates() {
    coop::nameplate::Snapshot ns;
    coop::nameplate::GetSnapshot(ns);
    if (ns.count <= 0) return;
    // BACKGROUND draw list: over the game scene but UNDER every ImGui window
    // (user 2026-06-12: nameplates are the lowest-priority layer -- the scoreboard /
    // voice panel / browser must never be overdrawn by a plate). It renders in the
    // same ImDrawData as everything else, so the screenshot grab still captures it;
    // it never hit-tests input.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < ns.count; ++i) {
        const auto& p = ns.plates[i];
        if (!p.onScreen || p.alpha <= 0.02f) continue;
        DrawNameplate(dl, p);
    }
}

// Dev object-overlay labels (coop::dev::object_overlay snapshot): a small anchor
// dot at each projected object + up to 3 stacked text lines (names / net identity
// / physics layers -- empty lines are skipped). Line 1 is tinted by tracking kind
// so the problem class jumps out at a glance: cyan = wire mirror, green = tracked
// local, ORANGE = untracked local-only (the objects that cannot mirror-sync).
void DrawObjectOverlay() {
    namespace OO = coop::dev::object_overlay;
    if (!OO::IsEnabled()) return;
    OO::Snapshot os;
    OO::GetSnapshot(os);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under windows, like the nameplates
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();

    // Always-on status line (top-right): proves the overlay is live + shows the
    // in-range tracked/untracked counts even when no label is on screen.
    if (os.status[0]) {
        constexpr float kStatusPx = 13.f;
        const ImVec2 sz = font->CalcTextSizeA(kStatusPx, FLT_MAX, 0.f, os.status);
        TextOutlined(dl, font, kStatusPx,
                     ImVec2(io.DisplaySize.x - sz.x - 10.f, 8.f),
                     IM_COL32(255, 235, 130, 235), IM_COL32(0, 0, 0, 200), os.status);
    }

    for (int i = 0; i < os.count; ++i) {
        const auto& L = os.labels[i];
        if (L.alpha <= 0.02f) continue;
        // Cull labels projected outside the viewport (small margin keeps a label
        // attached to an object sliding off the edge from popping).
        if (L.x < -60.f || L.y < -60.f ||
            L.x > io.DisplaySize.x + 60.f || L.y > io.DisplaySize.y + 60.f) continue;

        // Same billboard shape as the nameplates: base size up close, ~1/distance
        // shrink to a legible floor.
        const float s = (L.dist <= 600.f) ? 1.f : std::max(600.f / L.dist, 0.45f);
        const float a = std::clamp(L.alpha, 0.f, 1.f);

        ImU32 kindCol;
        switch (L.kind) {
            case 0:  kindCol = IM_COL32(110, 205, 255, static_cast<int>(a * 245.f)); break;
            case 1:  kindCol = IM_COL32(150, 255, 150, static_cast<int>(a * 245.f)); break;
            default: kindCol = IM_COL32(255, 170, 64,  static_cast<int>(a * 255.f)); break;
        }
        const ImU32 grey    = IM_COL32(225, 225, 225, static_cast<int>(a * 225.f));
        const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 210.f));

        dl->AddCircleFilled(ImVec2(L.x, L.y), 2.5f * s, kindCol);

        const float px1 = 13.f * s, px2 = 11.f * s;
        float tx = L.x + 7.f * s;
        float ty = L.y - 6.f * s;
        if (L.line1[0]) { TextOutlined(dl, font, px1, ImVec2(tx, ty), kindCol, outline, L.line1); ty += px1 + 1.f; }
        if (L.line2[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line2);   ty += px2 + 1.f; }
        if (L.line3[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line3); }
    }
}

void DrawChat() {
    coop::chat_feed::Snapshot cs;
    coop::chat_feed::GetSnapshot(cs);
    if (cs.count <= 0) return;

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float pad = 14.f;
    // Anchor the window's BOTTOM-LEFT corner at the left edge, RAISED to ~mid-screen height.
    // The chat used to sit in the bottom-left corner; the user (2026-06-08) wanted it higher,
    // around the middle. Pivot (0,1) keeps it growing UPWARD as lines stack (newest at the
    // bottom), so the bottom edge sits at kBottomFrac of the screen height.
    constexpr float kBottomFrac = 0.5f;  // 0.5 = vertical middle; raise toward 0 / lower toward 1
    ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y * kBottomFrac), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##coop_chat", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < cs.count; ++i) {
            const auto& l = cs.lines[i];
            const float a = std::clamp(l.alpha, 0.f, 1.f);
            // Per-line drop shadow so text reads over a bright scene (the window has
            // no background of its own).
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f),
                        IM_COL32(0, 0, 0, static_cast<int>(a * 190.f)), l.text);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(236, 236, 236, static_cast<int>(a * 235.f)));
            ImGui::TextUnformatted(l.text);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

}  // namespace

bool IsActive() {
    return coop::nameplate::HasAny() || coop::chat_feed::HasAny() ||
           coop::dev::object_overlay::IsEnabled() ||
           coop::voice_chat::Enabled();  // v66: the local mic indicator works pre-join too
}

// The local voice indicator (v66): a compact bottom-left icon, the SVC HUD chain
// (talking / whispering / muted / disconnected; PTT idle shows nothing). Reads
// the published UiSnapshot -- this runs on the RENDER thread, and the live
// chain (LocalHudIcon) walks game-thread state (audit I-1).
void DrawLocalVoiceIcon() {
    coop::voice_chat::UiSnapshot vs;
    coop::voice_chat::GetUiSnapshot(vs);
    if (!vs.enabled || !vs.started) return;
    const auto icon = static_cast<coop::voice_chat::VoiceIcon>(vs.localIcon);
    if (icon == coop::voice_chat::VoiceIcon::None) return;
    // Hide the "voice disconnected" badge when solo (no remote players present): a
    // lone host/client hasn't failed at anything -- there is simply nobody to talk to
    // yet, so the "no signal" glyph was pure noise (it vanished the instant a peer
    // joined, which is exactly what the user saw). Once a peer IS present the icon is
    // meaningful again (a real voice-transport-down state). (user 2026-06-18)
    if (icon == coop::voice_chat::VoiceIcon::Disconnected && !coop::nameplate::HasAny()) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under windows, like the nameplates
    // Offset right of the far-left edge so it clears VOTV's native bottom-left vitals
    // column (food / stamina icons + their numbers), which the old x=26 fought with
    // (user, 2026-06-13). x=170 clears the vitals readout; a compact 28 px badge
    // sat near the bottom edge (user 2026-06-18: the prior 72 px badge was far too
    // large).
    ui::voice_icons::Draw(dl, ImVec2(170.f, io.DisplaySize.y - 36.f), 28.f, icon, 0.9f);
}

void Render() {
    DrawObjectOverlay();   // first: debug labels sit UNDER the player nameplates
    DrawNameplates();
    DrawChat();
    DrawLocalVoiceIcon();
}

}  // namespace ui::hud
