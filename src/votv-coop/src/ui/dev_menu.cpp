// ui/dev_menu.cpp -- see ui/dev_menu.h.

#include "ui/dev_menu.h"

#include "coop/dev/force_weather.h"
#include "coop/ini_config.h"

#include <vector>

#include "imgui.h"

namespace ui::dev_menu {
namespace {

// One control in the content pane. `render` draws its own ImGui widget(s) so an
// item can be a button, a checkbox reflecting live state, a slider, etc. `dev`
// hides it unless the dev switch is on.
struct Item { void (*render)(); bool dev; };
struct Sub  { const char* name; std::vector<Item> items; bool dev; };
struct Cat  { const char* name; std::vector<Sub> subs;  bool dev; };

// ---- feature controls (each calls a plain function its module exposes) -------

void RenderSnow() {
    bool on = coop::dev::force_weather::IsSnowOn();
    // Host-authoritative: the toggle is a no-op on a client (the module gates it).
    if (ImGui::Checkbox("Snow", &on)) coop::dev::force_weather::SetSnow(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(host-authoritative; clients mirror)");
}

// ---- the strict nested taxonomy (refined as features land) -------------------
// Player > Movement/Vitals/HUD ; Game > Weather/Entities/Events ; Network >
// Stats/Session ; Cosmetics > Skins. Only Weather>Snow is wired so far (the
// foundation's proof feature); the rest are placeholders migrated in follow-ups.
// Cosmetics is the one non-dev category (shown to all players).
const std::vector<Cat>& Tree() {
    static const std::vector<Cat> kTree = {
        { "Player", {
            { "Movement", {}, true },
            { "Vitals",   {}, true },
            { "HUD",      {}, true },
        }, true },
        { "Game", {
            { "Weather",  { { &RenderSnow, true } }, true },
            { "Entities", {}, true },
            { "Events",   {}, true },
        }, true },
        { "Network", {
            { "Stats",    {}, true },
            { "Session",  {}, true },
        }, true },
        { "Cosmetics", {
            { "Skins",    {}, false },
        }, false },
    };
    return kTree;
}

const Cat* g_selCat = nullptr;
const Sub* g_selSub = nullptr;

// Dev switch, read ONCE at boot by Init() (off the render thread). devMode=false
// -> only non-dev (Cosmetics) shows; the menu itself is always available.
bool g_devMode = false;

}  // namespace

void Init() {
    g_devMode = ::coop::ini_config::IsIniKeyTrue("devkeys");
}

void Render() {
    const bool devMode = g_devMode;
    const auto& tree = Tree();

    ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VOTV Coop  -  Menu (F1)", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Left: nav tree. A major category renders as a FRAMED, accent-coloured section
    // bar (solid slate frame + light-blue label + taller row) so it clearly outranks
    // its plain, indented sub-category items below. NoTreePushOnOpen -> we own the
    // child indent explicitly (no TreePop bookkeeping).
    ImGui::BeginChild("##nav", ImVec2(185, 0), ImGuiChildFlags_Borders);
    bool firstCat = true;
    for (const auto& cat : tree) {
        if (cat.dev && !devMode) continue;
        if (!firstCat) ImGui::Spacing();
        firstCat = false;
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.20f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.30f, 0.38f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.30f, 0.35f, 0.45f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.55f, 0.82f, 1.00f, 1.00f));
        const bool open = ImGui::TreeNodeEx(cat.name,
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen |
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopStyleColor(4);
        if (!open) continue;
        ImGui::Indent(14.0f);
        for (const auto& sub : cat.subs) {
            if (sub.dev && !devMode) continue;
            const bool selected = (g_selSub == &sub);
            if (ImGui::Selectable(sub.name, selected)) { g_selCat = &cat; g_selSub = &sub; }
        }
        ImGui::Unindent(14.0f);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: the selected subcategory's controls.
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_selCat && g_selSub) {
        ImGui::TextDisabled("%s  >  %s", g_selCat->name, g_selSub->name);
        ImGui::Separator();
        int shown = 0;
        for (const auto& it : g_selSub->items) {
            if (it.dev && !devMode) continue;
            if (it.render) { it.render(); ++shown; }
        }
        if (shown == 0) ImGui::TextDisabled("No tools here yet -- coming soon.");
    } else {
        ImGui::TextDisabled("Select a category on the left.");
        if (!devMode) {
            ImGui::Spacing();
            ImGui::TextWrapped("Developer tools are hidden. Set [dev] devkeys=1 in "
                               "votv-coop.ini to show them.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace ui::dev_menu
