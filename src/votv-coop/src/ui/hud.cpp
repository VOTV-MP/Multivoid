// ui/hud.cpp -- see ui/hud.h.

#include "ui/hud.h"

#include "coop/chat_feed.h"
#include "coop/nameplate.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace ui::hud {
namespace {

// Nameplate text size (px). Bigger than ImGui's ~13px default so a nick reads at a
// few metres + survives the downscale of an autonomous screenshot.
constexpr float kNickPx = 22.f;

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
    if (p.ping > 0) std::snprintf(line, sizeof(line), "%s (%dms)", p.nick, p.ping);
    else            std::snprintf(line, sizeof(line), "%s", p.nick);

    const float a = std::clamp(p.alpha, 0.f, 1.f);
    const ImU32 white   = IM_COL32(255, 255, 255, static_cast<int>(a * 245.f));
    const ImU32 red     = IM_COL32(255, 48, 48, static_cast<int>(a * 255.f));
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 215.f));
    const ImU32 textCol = p.flash ? red : white;

    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(kNickPx, FLT_MAX, 0.f, line);
    constexpr float barW = 60.f, barH = 6.f;
    // Keep the whole nameplate on-screen: a peer in FRONT of you but whose head sits
    // past a screen edge still shows the label at the edge instead of vanishing. The
    // nick sits sz.y+10 above the anchor; the bar barH+4 below.
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float m = 6.f;
    const float halfW = std::max(sz.x, barW) * 0.5f;
    const float ax = std::clamp(p.x, m + halfW, io.DisplaySize.x - m - halfW);
    const float ay = std::clamp(p.y, m + sz.y + 10.f, io.DisplaySize.y - m - barH - 4.f);
    const ImVec2 textPos(ax - sz.x * 0.5f, ay - sz.y - 10.f);
    const ImVec2 bp(ax - barW * 0.5f, ay - 4.f);

    // Readability backing: a translucent rounded box behind the nick + bar so the
    // label reads over ANY scene (bright sky, white wall, foliage).
    constexpr float padX = 6.f, padY = 3.f;
    const ImVec2 boxMin(std::min(textPos.x, bp.x) - padX, textPos.y - padY);
    const ImVec2 boxMax(std::max(textPos.x + sz.x, bp.x + barW) + padX, bp.y + barH + padY);
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, static_cast<int>(a * 140.f)), 4.f);

    TextOutlined(dl, font, kNickPx, textPos, textCol, outline, line);

    // Health bar (dark red).
    const float frac = std::clamp(p.healthPct / 100.f, 0.f, 1.f);
    dl->AddRectFilled(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 160.f)));
    const ImU32 fillCol = p.flash ? red : IM_COL32(190, 30, 30, static_cast<int>(a * 235.f));
    dl->AddRectFilled(bp, ImVec2(bp.x + barW * frac, bp.y + barH), fillCol);
    dl->AddRect(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 200.f)));
}

void DrawNameplates() {
    coop::nameplate::Snapshot ns;
    coop::nameplate::GetSnapshot(ns);
    if (ns.count <= 0) return;
    // FOREGROUND draw list: drawn ON TOP of the scene (and any ImGui window); never
    // hit-tests input. (Was the background list -- foreground guarantees it's never
    // occluded + is captured by the screenshot grab like every other ImGui surface.)
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < ns.count; ++i) {
        const auto& p = ns.plates[i];
        if (!p.onScreen || p.alpha <= 0.02f) continue;
        DrawNameplate(dl, p);
    }
}

void DrawChat() {
    coop::chat_feed::Snapshot cs;
    coop::chat_feed::GetSnapshot(cs);
    if (cs.count <= 0) return;

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float pad = 14.f;
    // Anchor the window's BOTTOM-LEFT corner near the bottom-left of the screen; it
    // auto-resizes upward as lines stack (newest at the bottom).
    ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y - pad), ImGuiCond_Always, ImVec2(0.f, 1.f));
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
    return coop::nameplate::HasAny() || coop::chat_feed::HasAny();
}

void Render() {
    DrawNameplates();
    DrawChat();
}

}  // namespace ui::hud
