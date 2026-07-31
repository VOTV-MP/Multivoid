// ui/hud.cpp -- see ui/hud.h.

#include "ui/hud.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/player/nameplate.h"
#include "coop/text/utf8_codec.h"
#include "coop/player/nick_color.h"
#include "coop/voice/voice_chat.h"
#include "ui/chat_view.h"
#include "ui/fonts.h"
#include "ui/link_format.h"
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
    // A plate has only ONE parenthetical slot, so it cannot say "not applicable"
    // the way the scoreboard's cell can -- it can only show a number or nothing.
    // A ping < 0 therefore renders as a bare name, which now means only "not sampled
    // yet". It used to ALSO mean the host, whose own row publishes -1 because there is
    // no RTT to itself to report -- but a client no longer reads that row: it reads its
    // own, via roster_ledger::DisplayLink, because host<->client is one link with one
    // RTT (user 2026-07-31: "client should see a Host <X> too"). Host-side, the host's
    // own row is still Local/-1, and you never see your own plate anyway. The number
    // itself is formatted by the one shared renderer.
    //
    // The name and the ping are still drawn as two pieces (they carry one colour
    // each), but they are CENTRED AS ONE UNIT -- see the anchor block below, which
    // records the 2026-07-31 user decision that supersedes the 2026-07-28 one.
    // Kept for the record, because it is the cost the user accepted: centring
    // `"<nick> (<ping>)"` as a unit means the NICK's centre sits half the suffix's
    // width left of the bar's centre, and the name slides sideways as latency gains
    // or loses a digit. 2026-07-28 that was the complaint ("никнейм не идеально
    // прилегает своим центром к полоске hp, а выперает слева"); 2026-07-31, shown
    // the result, the user asked for the whole label centred instead.
    char nickTxt[coop::text::kNickBufBytes];
    std::snprintf(nickTxt, sizeof(nickTxt), "%s", p.nick);
    char pingTxt[20] = {};
    if (p.linkKind != coop::net::LinkKind::Local && p.ping >= 0) {
        char pb[16];
        ui::link_format::FormatPing(p.ping, p.linkKind, pb, sizeof(pb));
        std::snprintf(pingTxt, sizeof(pingTxt), " (%s)", pb);
    }

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

    ImFont* font = ui::fonts::FontFor(ui::fonts::Role::Nameplate);  // per-role font (F1 > Interface)
    if (!font) font = ImGui::GetFont();
    // Measured SEPARATELY because they carry one colour each and are drawn as two
    // calls -- NOT because the anchor centres the nick alone; it no longer does.
    const ImVec2 sz     = font->CalcTextSizeA(px, FLT_MAX, 0.f, nickTxt);
    const ImVec2 pingSz = pingTxt[0] ? font->CalcTextSizeA(px, FLT_MAX, 0.f, pingTxt)
                                     : ImVec2(0.f, 0.f);

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
            const char* rowEnd = font->CalcWordWrapPositionA(bpx / font->LegacySize, sPtr, bEnd, wrapW);
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
    // The WHOLE label is centred on the bar -- nick AND ping as one unit, so the
    // plate is symmetric about the anchor again.
    //
    // USER DECISION 2026-07-31, and it SUPERSEDES the 2026-07-28 one recorded above.
    // Shown the live plate, the user reported "Client <1ms> is not centered on the
    // health bar" and, asked to choose between centring the whole string, moving the
    // ping to its own line, or dropping it, answered "1. Center all actually". The
    // known cost is the one 2026-07-04 objected to: the nick shifts a few pixels when
    // the ping gains or loses a digit. That is accepted, not overlooked -- do not
    // "fix" it back to nick-only centring without asking.
    //
    // Same shape as MTA, which draws the whole nametag string with DT_CENTER on the
    // X the health bar is centred on (Client/mods/deathmatch/logic/CNametags.cpp:281).
    const float labelW = sz.x + pingSz.x;
    const float ext = std::max(labelW, barW) * 0.5f;
    const float loX = m + ext, hiX = io.DisplaySize.x - m - ext;
    const float ax = (loX <= hiX) ? std::clamp(p.x, loX, hiX) : p.x;
    const float ay = std::clamp(p.y, m + sz.y + gap + bubbleH, io.DisplaySize.y - m - barH - barGap);
    const ImVec2 textPos(ax - labelW * 0.5f, ay - sz.y - gap);
    const ImVec2 bp(ax - barW * 0.5f, ay - barGap);

    // Plate extents (nick + bar union), kept for the voice-badge anchor below. The
    // translucent black backing box once drawn from these extents is REMOVED (user
    // 2026-07-02: no black rectangle behind the plate; if it ever returns, restore
    // the AddRectFilled from git history) -- readability rides the 1px text outline
    // + the health bar's own outline.
    const float padX = S(6.f) * s, padY = S(3.f) * s;
    const ImVec2 boxMin(std::min(textPos.x, bp.x) - padX, textPos.y - padY);
    const ImVec2 boxMax(std::max(textPos.x + sz.x + pingSz.x, bp.x + barW) + padX,
                        bp.y + barH + padY);

    TextOutlined(dl, font, px, textPos, textCol, outline, nickTxt);
    // The annotation still hangs off the nick's right edge -- but the pair is now
    // centred as one unit (see the anchor above), so this is a layout continuation,
    // not an exemption from the centring.
    if (pingTxt[0]) {
        TextOutlined(dl, font, px, ImVec2(textPos.x + sz.x, textPos.y),
                     textCol, outline, pingTxt);
    }

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
        if (L.line3[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line3);   ty += px2 + S(1.f); }
        if (L.line4[0]) {
            // Health/process ramp: green (full) -> yellow -> red (empty); grey
            // when no max is known (a bare pool has no meaningful fraction).
            ImU32 hcol = grey;
            if (L.healthFrac >= 0.f) {
                const float f = L.healthFrac;
                const int rr = static_cast<int>(255.f * std::min(1.f, 2.f - 2.f * f));
                const int gg = static_cast<int>(255.f * std::min(1.f, 2.f * f));
                hcol = IM_COL32(rr, gg, 40, static_cast<int>(a * 245.f));
            }
            TextOutlined(dl, font, px2, ImVec2(tx, ty), hcol, outline, L.line4);
        }
    }
}

}  // namespace

bool IsActive() {
    // chat_feed::HasAny() is LIVE lines only. RevealActive() is the other half: while
    // the T-history is on screen -- including the fade-out after a close -- the frame
    // must keep being built even if every live line has already expired, or the fade
    // draws zero frames and the block vanishes instead of dimming.
    return coop::nameplate::HasAny() || coop::chat_feed::HasAny() ||
           coop::chat_feed::RevealActive() ||
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
    ui::chat_view::Draw();  // the feed + the T-activated history reveal (ui/chat_view.h)
    DrawLocalVoiceIcon();
}

}  // namespace ui::hud
