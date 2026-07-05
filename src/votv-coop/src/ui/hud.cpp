// ui/hud.cpp -- see ui/hud.h.

#include "ui/hud.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/voice/voice_chat.h"
#include "ui/fonts.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace ui::hud {
namespace {

using ui::scale::S;  // 1080p-authored px -> live-resolution px (ui/scale.h)

// Nameplate BASE text size (px) -- the size up CLOSE. The whole plate scales DOWN with
// distance (p.scale, ~1/distance, computed in coop::nameplate::DistanceScale) so it stays
// proportional to the peer's on-screen body instead of looming over a far, shrunken
// character (user 2026-06-08: the old fixed size "grew" relative to a receding peer).
// 16 px up close reads at a few metres without dominating when a peer stands right next
// to you (user 2026-06-08: 22 px felt huge up close). Distance also fades OPACITY
// (DistanceAlpha); p.scale is capped at 1.0 so the plate is never bigger than this.
constexpr float kNickPx = 16.f;

// Draw `text` at `size` with a cheap outline (4-way offset, ~1px at 1080p and
// proportional above) so it stays legible over any scene.
void TextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                  ImU32 col, ImU32 outline, const char* text) {
    const float o = std::max(1.f, S(1.f));
    dl->AddText(font, size, ImVec2(pos.x - o, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x + o, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y - o), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y + o), outline, text);
    dl->AddText(font, size, pos, col, text);
}

void DrawNameplate(ImDrawList* dl, const coop::nameplate::Plate& p) {
    char line[64];
    if (p.ping > 0)       std::snprintf(line, sizeof(line), "%s (%dms)", p.nick, p.ping);
    else if (p.ping == 0) std::snprintf(line, sizeof(line), "%s (<1ms)", p.nick);  // sub-ms LAN
    else                  std::snprintf(line, sizeof(line), "%s", p.nick);          // -1 = unmeasured

    // Occlusion (minecraft nametag shape; user 2026-07-05 refining 07-04): a peer
    // behind world geometry keeps a readable plate, but the WHOLE unit -- nick AND
    // health bar (badge/outline ride the same `a`) -- goes GRAY + half-transparent
    // (x0.5) so "behind something" reads at a glance and nothing vanishes outright.
    // Hurt-flash red keeps priority -- a hurt peer stays visible either way.
    const float a = std::clamp(p.alpha, 0.f, 1.f) * (p.occluded ? 0.5f : 1.f);
    const ImU32 white   = IM_COL32(255, 255, 255, static_cast<int>(a * 245.f));
    const ImU32 gray    = IM_COL32(158, 158, 164, static_cast<int>(a * 245.f));
    const ImU32 red     = IM_COL32(255, 48, 48, static_cast<int>(a * 255.f));
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 215.f));
    // v103 (12f): the peer's custom nick color replaces the white base; the
    // flash/occluded signal colors keep priority (a hurt or hidden peer must
    // read as such regardless of the cosmetic pick).
    const ImU32 base = coop::nick_color::IsCustom(p.colorRGB)
        ? IM_COL32(coop::nick_color::R(p.colorRGB), coop::nick_color::G(p.colorRGB),
                   coop::nick_color::B(p.colorRGB), static_cast<int>(a * 245.f))
        : white;
    const ImU32 textCol = p.flash ? red : (p.occluded ? gray : base);

    // Distance SIZE scale: the whole plate (text + bar + box + gaps) scales as ONE unit
    // so a far peer's label stays proportional to their shrunken on-screen body. p.scale
    // is 1.0 up close (capped) and shrinks ~1/distance to a legible floor far away.
    const float s = std::clamp(p.scale, 0.20f, 1.f);
    const float px = S(kNickPx) * s;
    const float barW = S(44.f) * s, barH = S(5.f) * s;
    const float gap = S(10.f) * s;    // nick sits sz.y + gap above the head anchor
    const float barGap = S(4.f) * s;  // bar sits barGap below the anchor

    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.f, line);

    // 12g overhead chat bubble rows -- split BEFORE the on-screen clamp so the
    // clamp can reserve the bubble's height (audit 2026-07-05: an unclamped
    // bubble stack rendered off-screen-top when looking up at a nearby peer).
    const float bpx = px * 0.88f;
    const float rowH = bpx + S(1.f) * s;
    constexpr int kMaxRows = 5;
    struct BubbleRow { const char* b; const char* e; };
    BubbleRow rows[kMaxRows];
    int nRows = 0;
    if (p.bubbleAlpha > 0.f && p.bubble[0]) {
        const float wrapW = S(240.f) * s;
        const char* sPtr = p.bubble;
        const char* bEnd = p.bubble + std::strlen(p.bubble);
        while (sPtr < bEnd && nRows < kMaxRows) {
            const char* rowEnd = font->CalcWordWrapPositionA(bpx / font->FontSize, sPtr, bEnd, wrapW);
            if (rowEnd == sPtr) rowEnd = sPtr + 1;  // never stall on a single overlong glyph
            rows[nRows++] = {sPtr, rowEnd};
            sPtr = rowEnd;
            while (sPtr < bEnd && *sPtr == ' ') ++sPtr;  // swallow the wrap-point space
        }
    }
    const float bubbleH = nRows > 0 ? rowH * static_cast<float>(nRows) + S(4.f) * s : 0.f;

    // Keep the whole nameplate on-screen: a peer in FRONT of you but whose head sits
    // past a screen edge still shows the label at the edge instead of vanishing.
    const ImGuiIO& io = ImGui::GetIO();
    const float m = S(6.f);
    const float halfW = std::max(sz.x, barW) * 0.5f;
    const float ax = std::clamp(p.x, m + halfW, io.DisplaySize.x - m - halfW);
    const float ay = std::clamp(p.y, m + sz.y + gap + bubbleH, io.DisplaySize.y - m - barH - barGap);
    const ImVec2 textPos(ax - sz.x * 0.5f, ay - sz.y - gap);
    const ImVec2 bp(ax - barW * 0.5f, ay - barGap);

    // Plate extents (nick + bar union), kept for the voice-badge anchor below. The
    // translucent black backing box once drawn from these extents is REMOVED (user
    // 2026-07-02: no black rectangle behind the plate; if it ever returns, restore
    // the AddRectFilled from git history) -- readability rides the 1px text outline
    // + the health bar's own outline.
    const float padX = S(6.f) * s, padY = S(3.f) * s;
    const ImVec2 boxMin(std::min(textPos.x, bp.x) - padX, textPos.y - padY);
    const ImVec2 boxMax(std::max(textPos.x + sz.x, bp.x + barW) + padX, bp.y + barH + padY);

    TextOutlined(dl, font, px, textPos, textCol, outline, line);

    // 12g overhead chat bubble (MTA/SAMP shape): the peer's last chat message,
    // word-wrapped + centered ABOVE the nick, its own hold/fade (chat_bubbles)
    // multiplied into the plate's distance/occlusion alpha. Outlined text only --
    // no backing box, consistent with the plate (user 2026-07-02: no black
    // rectangles). Rows were split above (before the clamp, which reserved
    // bubbleH); capped so a max-length line can't tower over the scene.
    if (nRows > 0) {
        const float ba = a * std::clamp(p.bubbleAlpha, 0.f, 1.f);
        const ImU32 bCol = IM_COL32(255, 255, 255, static_cast<int>(ba * 235.f));
        const ImU32 bOut = IM_COL32(0, 0, 0, static_cast<int>(ba * 205.f));
        float y = textPos.y - S(4.f) * s - rowH * static_cast<float>(nRows);
        for (int r = 0; r < nRows; ++r) {
            char buf[224];
            const size_t len = std::min(static_cast<size_t>(rows[r].e - rows[r].b),
                                        sizeof(buf) - 1);
            std::memcpy(buf, rows[r].b, len);
            buf[len] = '\0';
            const ImVec2 rsz = font->CalcTextSizeA(bpx, FLT_MAX, 0.f, buf);
            TextOutlined(dl, font, bpx, ImVec2(ax - rsz.x * 0.5f, y), bCol, bOut, buf);
            y += rowH;
        }
    }

    // Health bar (dark red). While occluded the fill stays RED -- a darker,
    // more transparent red than the normal fill (user 2026-07-05: not gray;
    // "hp остаётся красным, но потемнее и полупрозрачным" behind objects).
    // Note `a` already carries the occlusion x0.5, so the lower constant here
    // stacks on that. The nick keeps the gray treatment; hurt-flash wins.
    const float frac = std::clamp(p.healthPct / 100.f, 0.f, 1.f);
    dl->AddRectFilled(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 160.f)));
    const ImU32 fillCol = p.flash    ? red
                        : p.occluded ? IM_COL32(120, 18, 18, static_cast<int>(a * 220.f))
                                     : IM_COL32(190, 30, 30, static_cast<int>(a * 235.f));
    dl->AddRectFilled(bp, ImVec2(bp.x + barW * frac, bp.y + barH), fillCol);
    dl->AddRect(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 200.f)));

    // Voice badge (v66): right of the plate backing, scaled+faded with it
    // (the SVC nameplate icon placement, design SS3.1).
    if (p.voiceIcon != 0) {
        const float ih = S(13.f) * s;
        ui::voice_icons::Draw(dl, ImVec2(boxMax.x + S(4.f) * s + ih * 0.5f,
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
        const float statusPx = S(13.f);
        const ImVec2 sz = font->CalcTextSizeA(statusPx, FLT_MAX, 0.f, os.status);
        TextOutlined(dl, font, statusPx,
                     ImVec2(io.DisplaySize.x - sz.x - S(10.f), S(8.f)),
                     IM_COL32(255, 235, 130, 235), IM_COL32(0, 0, 0, 200), os.status);
    }

    for (int i = 0; i < os.count; ++i) {
        const auto& L = os.labels[i];
        if (L.alpha <= 0.02f) continue;
        // Cull labels projected outside the viewport (small margin keeps a label
        // attached to an object sliding off the edge from popping).
        const float cm = S(60.f);
        if (L.x < -cm || L.y < -cm ||
            L.x > io.DisplaySize.x + cm || L.y > io.DisplaySize.y + cm) continue;

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

        dl->AddCircleFilled(ImVec2(L.x, L.y), S(2.5f) * s, kindCol);

        const float px1 = S(13.f) * s, px2 = S(11.f) * s;
        float tx = L.x + S(7.f) * s;
        float ty = L.y - S(6.f) * s;
        if (L.line1[0]) { TextOutlined(dl, font, px1, ImVec2(tx, ty), kindCol, outline, L.line1); ty += px1 + S(1.f); }
        if (L.line2[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line2);   ty += px2 + S(1.f); }
        if (L.line3[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line3); }
    }
}

void DrawChat() {
    coop::chat_feed::Snapshot cs;
    coop::chat_feed::GetSnapshot(cs);
    if (cs.count <= 0) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float pad = S(14.f);
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
    // 2026-07-04 chat-imgui-samp adoption: bold chat font (Cyrillic-capable), per-slot
    // colored nick prefix, 4-way outline (replaces the 1px shadow -- reads over any
    // scene), word-wrap at a fixed column width, and the feed's new fade-IN ramp.
    ImFont* font = ui::fonts::Chat();
    if (!font) font = ImGui::GetFont();
    // ChatPx() = the px the chat font was BAKED at (scaled): drawing at exactly
    // that size renders the crisp 1:1 rasterization, no bitmap resample.
    const float px = ui::fonts::Chat() ? ui::fonts::ChatPx() : ImGui::GetFontSize();
    const float wrapW = std::min(io.DisplaySize.x * 0.42f, S(640.f));

    // Per-slot nick palette (8 entries, wraps). Matches the scoreboard's hue family:
    // distinct, readable on dark AND bright scenes at full alpha.
    static constexpr ImU32 kSlotCols[] = {
        IM_COL32(255, 179,  64, 255),  // 0 host: amber
        IM_COL32(110, 205, 255, 255),  // 1 sky
        IM_COL32(150, 255, 150, 255),  // 2 mint
        IM_COL32(255, 130, 160, 255),  // 3 rose
        IM_COL32(200, 150, 255, 255),  // 4 lilac
        IM_COL32(255, 240, 120, 255),  // 5 lemon
        IM_COL32(120, 235, 220, 255),  // 6 teal
        IM_COL32(255, 160, 110, 255),  // 7 coral
    };

    if (ImGui::Begin("##coop_chat", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < cs.count; ++i) {
            const auto& l = cs.lines[i];
            const float a = std::clamp(l.alpha, 0.f, 1.f);
            const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 200.f));
            const ImU32 body    = IM_COL32(236, 236, 236, static_cast<int>(a * 245.f));
            ImU32 nickCol = body;
            if (l.nickLen > 0) {
                // v103 (12f): a peer's custom nick color overrides the per-slot
                // palette; unset -> the palette as before.
                const uint32_t custom = coop::nick_color::PackedForSlot(l.slot);
                const ImU32 c = coop::nick_color::IsCustom(custom)
                    ? IM_COL32(coop::nick_color::R(custom), coop::nick_color::G(custom),
                               coop::nick_color::B(custom), 255)
                    : kSlotCols[l.slot % (sizeof(kSlotCols) / sizeof(kSlotCols[0]))];
                nickCol = (c & 0x00FFFFFFu) | (static_cast<ImU32>(a * 255.f) << 24);
            }

            // Manual wrap + colored-segment draw. Each visual row advances the
            // ImGui cursor (Dummy) so the auto-resize window grows to fit.
            const char* s   = l.text;
            const char* end = l.text + std::strlen(l.text);
            const char* nickEnd = l.text + ((l.nickLen && l.nickLen < std::strlen(l.text))
                                            ? l.nickLen : 0);
            const float o = std::max(1.f, S(1.f));  // outline offset (see TextOutlined)
            while (s < end) {
                const char* rowEnd = font->CalcWordWrapPositionA(px / font->FontSize, s, end, wrapW);
                if (rowEnd == s) rowEnd = s + 1;  // never stall on a single overlong glyph
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                float x = pos.x;
                const char* seg = s;
                // Split the row at the nick boundary when it falls inside this row.
                while (seg < rowEnd) {
                    const char* segEnd = (seg < nickEnd && nickEnd < rowEnd) ? nickEnd : rowEnd;
                    const ImU32 col = (seg < nickEnd) ? nickCol : body;
                    dl->AddText(font, px, ImVec2(x - o, pos.y), outline, seg, segEnd);
                    dl->AddText(font, px, ImVec2(x + o, pos.y), outline, seg, segEnd);
                    dl->AddText(font, px, ImVec2(x, pos.y - o), outline, seg, segEnd);
                    dl->AddText(font, px, ImVec2(x, pos.y + o), outline, seg, segEnd);
                    dl->AddText(font, px, ImVec2(x, pos.y), col, seg, segEnd);
                    x += font->CalcTextSizeA(px, FLT_MAX, 0.f, seg, segEnd).x;
                    seg = segEnd;
                }
                ImGui::Dummy(ImVec2(x - pos.x, px + S(2.f)));
                s = rowEnd;
                while (s < end && *s == ' ') ++s;  // swallow the wrap-point space
            }
        }
    }
    ImGui::End();
}

}  // namespace

bool IsActive() {
    return coop::nameplate::HasAny() || coop::chat_feed::HasAny() ||
           coop::dev::object_overlay::IsEnabled() ||
           coop::dev::ragdoll_bone_overlay::IsEnabled() ||
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
    ui::voice_icons::Draw(dl, ImVec2(S(170.f), io.DisplaySize.y - S(36.f)), S(28.f), icon, 0.9f);
}

// Dev ragdoll skeleton ESP (coop::dev::ragdoll_bone_overlay snapshot): bone->parent
// lines + joint dots for every ACTIVE ragdoll body. Orange = the local player's own
// native ragdoll; cyan = a remote peer's mirror body (the v22 pelvis-coupled one).
void DrawRagdollBones() {
    namespace RB = coop::dev::ragdoll_bone_overlay;
    if (!RB::IsEnabled()) return;
    RB::Snapshot rs;
    RB::GetSnapshot(rs);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();

    if (rs.status[0]) {
        const float statusPx = S(13.f);
        const ImVec2 sz = font->CalcTextSizeA(statusPx, FLT_MAX, 0.f, rs.status);
        TextOutlined(dl, font, statusPx,
                     ImVec2(io.DisplaySize.x - sz.x - S(10.f), S(26.f)),
                     IM_COL32(255, 190, 120, 235), IM_COL32(0, 0, 0, 200), rs.status);
    }

    for (int i = 0; i < rs.count; ++i) {
        const auto& L = rs.lines[i];
        // Cull segments fully outside the viewport (margin keeps partially-visible limbs).
        const float m = S(80.f);
        if ((L.x1 < -m && L.x2 < -m) || (L.y1 < -m && L.y2 < -m) ||
            (L.x1 > io.DisplaySize.x + m && L.x2 > io.DisplaySize.x + m) ||
            (L.y1 > io.DisplaySize.y + m && L.y2 > io.DisplaySize.y + m)) continue;
        const ImU32 col = (L.kind == 0) ? IM_COL32(255, 170, 64, 235)    // local native ragdoll
                                        : IM_COL32(110, 205, 255, 235);  // remote mirror body
        dl->AddLine(ImVec2(L.x1, L.y1), ImVec2(L.x2, L.y2), col, S(1.6f));
        dl->AddCircleFilled(ImVec2(L.x1, L.y1), S(2.2f), col);
        dl->AddCircleFilled(ImVec2(L.x2, L.y2), S(2.2f), col);
    }
}

void Render() {
    DrawObjectOverlay();   // first: debug labels sit UNDER the player nameplates
    DrawRagdollBones();    // skeleton lines under the nameplates too
    DrawNameplates();
    DrawChat();
    DrawLocalVoiceIcon();
}

}  // namespace ui::hud
