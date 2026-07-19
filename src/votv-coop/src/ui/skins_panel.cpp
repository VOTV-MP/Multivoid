// ui/skins_panel.cpp -- see ui/skins_panel.h.

#include "ui/skins_panel.h"

#include "coop/player/local_body.h"
#include "coop/player/skin_registry.h"
#include "ui/imgui_overlay.h"
#include "ui/scale.h"

#include <map>
#include <string>

#include "imgui.h"

namespace ui::skins_panel {
namespace {

struct Preview {
    void* srv = nullptr;  // ImTextureID (device-owned; dies with the device)
    int   w = 0, h = 0;
    bool  tried = false;  // one decode attempt per file per rescan
};

std::map<std::string, Preview> g_previews;
bool g_scanned = false;

constexpr float kTileW = 112.f;   // tile content width (1080p-authored; S() at use)
constexpr float kTileImgH = 112.f;

const Preview& PreviewFor(const coop::skins::SkinEntry& e) {
    Preview& p = g_previews[e.name];
    if (!p.tried) {
        p.tried = true;
        if (!e.previewPath.empty())
            p.srv = ui::imgui_overlay::CreateTextureFromImageFile(e.previewPath.c_str(), &p.w, &p.h);
    }
    return p;
}

}  // namespace

void Render() {
    if (!g_scanned) {
        g_scanned = true;
        coop::skins::Entries(true);
    }
    const auto& entries = coop::skins::Entries();
    const std::string current = coop::local_body::LocalSkinNameCopy();

    ImGui::TextWrapped("Your body skin -- what YOU see looking down and what OTHERS see. "
                       "Saved to multivoid.ini; restored on rejoin.");
    ImGui::TextDisabled("A skin = a converter .pak in Content/Paks/LogicMods/multivoid "
                        "(preview = <name>.png/.bmp next to it).");
    ImGui::TextDisabled("Peers WITHOUT that pak see the default kel body instead.");
    if (ImGui::Button("Refresh list")) {
        coop::skins::Entries(true);
        g_previews.clear();  // SRVs stay device-owned; a rescan re-creates as needed
    }
    ImGui::SameLine();
    ImGui::TextDisabled("current: %s", current.c_str());
    ImGui::Separator();

    // gmod-style tile grid: as many tiles per row as the pane fits.
    using ui::scale::S;
    const float tileW = S(kTileW), tileImgH = S(kTileImgH);
    const float availW = ImGui::GetContentRegionAvail().x;
    const float cellW = tileW + ImGui::GetStyle().ItemSpacing.x;
    int perRow = static_cast<int>(availW / cellW);
    if (perRow < 1) perRow = 1;

    int i = 0;
    for (const auto& e : entries) {
        if (i % perRow != 0) ImGui::SameLine();
        ++i;
        ImGui::PushID(e.name.c_str());
        ImGui::BeginGroup();

        const bool isCurrent = (e.name == current);
        const Preview& pv = PreviewFor(e);
        if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.58f, 0.28f, 1.0f));
        }
        bool clicked;
        if (pv.srv) {
            // Fit the image into the tile box preserving aspect.
            float iw = tileW, ih = tileImgH;
            if (pv.w > 0 && pv.h > 0) {
                const float s = (tileW / pv.w < tileImgH / pv.h) ? tileW / pv.w
                                                                 : tileImgH / pv.h;
                iw = pv.w * s; ih = pv.h * s;
            }
            // ImTextureID is ImU64 since 1.91.4 -- pointer goes through uintptr_t.
            clicked = ImGui::ImageButton("##tile",
                                         static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(pv.srv)),
                                         ImVec2(iw, ih));
        } else {
            clicked = ImGui::Button(e.name == coop::skins::kNativeSkinName ? "Dr. Kel\n(default)"
                                                                           : "(no preview)",
                                    ImVec2(tileW, tileImgH));
        }
        if (isCurrent) ImGui::PopStyleColor(3);
        // Name line under the tile, clipped to the tile width.
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + tileW);
        if (isCurrent) ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.6f, 1.0f), "%s", e.name.c_str());
        else           ImGui::TextUnformatted(e.name.c_str());
        ImGui::PopTextWrapPos();

        if (clicked && !isCurrent) coop::local_body::RequestSkin(e.name);
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

}  // namespace ui::skins_panel
