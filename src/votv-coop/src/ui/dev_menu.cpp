// ui/dev_menu.cpp -- see ui/dev_menu.h.

#include "ui/dev_menu.h"

#include "coop/dev/dev_gate.h"
#include "coop/dev/event_trigger.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/freecam.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/pos_hud.h"
#include "coop/dev/add_points.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/set_clock.h"
#include "coop/dev/spawn_menu_unlock.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dev/teleport_client.h"
#include "coop/ini_config.h"

#include <cctype>
#include <cstring>
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

void RenderTeleportClients() {
    if (ImGui::Button("Teleport clients to me")) coop::dev::teleport_client::TeleportClientsToHost();
    ImGui::SameLine();
    ImGui::TextDisabled("(host only)");
}

void RenderFreecam() {
    bool on = coop::dev::freecam::IsActive();
    if (ImGui::Checkbox("Freecam", &on)) coop::dev::freecam::SetActive(on);
    ImGui::TextDisabled("HOME also toggles - WASD move, Space/Ctrl up/down,");
    ImGui::TextDisabled("Shift fast, wheel speed, MMB bring player");
}

void RenderRestoreVitals() {
    if (ImGui::Button("Restore vitals (food / sleep / health)")) coop::dev::restore_vitals::Restore();
    ImGui::SameLine();
    ImGui::TextDisabled("(both peers)");
    if (ImGui::Button("Set vitals LOW (10)")) coop::dev::restore_vitals::SetLow();
    ImGui::SameLine();
    ImGui::TextDisabled("(local test -- food/sleep/health=10; nameplate syncs)");
}

void RenderSetClock() {
    namespace SC = coop::dev::set_clock;
    int day = 0; float frac = 0.f;
    if (!SC::ReadCurrent(day, frac)) {
        ImGui::TextDisabled("World clock not resolved yet (enter a world).");
        return;
    }
    ImGui::TextDisabled("Host-authoritative; the new day/time syncs to clients.");
    ImGui::Text("Now: day %d   time %.3f of day", day, frac);
    static int s_day = -1;
    if (s_day < 0) s_day = day;  // seed once from the live day
    ImGui::SetNextItemWidth(110.f);
    ImGui::InputInt("##clkday", &s_day);
    if (s_day < 0) s_day = 0;
    ImGui::SameLine();
    if (ImGui::Button("Set day")) SC::SetDay(s_day);
    ImGui::SameLine();
    ImGui::TextDisabled("(host only)");
    float f = frac;
    ImGui::SetNextItemWidth(260.f);
    if (ImGui::SliderFloat("Time of day", &f, 0.0f, 0.999f, "%.3f"))
        SC::SetTimeFraction(f);  // live: drag moves the sun (totalTime := frac * MaxTime)
    ImGui::TextDisabled("0 = day start  ->  ~1 = day end. Drag to move the sun.");
}

void RenderPosHud() {
    bool on = coop::dev::pos_hud::IsVisible();
    if (ImGui::Checkbox("Position / camera readout", &on)) coop::dev::pos_hud::SetVisible(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(on-screen overlay)");
}

void RenderObjectOverlay() {
    namespace OO = coop::dev::object_overlay;
    bool on = OO::IsEnabled();
    if (ImGui::Checkbox("World object overlay", &on)) OO::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(labels projected onto nearby objects)");
    ImGui::Indent(22.f);
    ImGui::BeginDisabled(!on);
    bool names = OO::LayerNames();
    if (ImGui::Checkbox("Object names", &names)) OO::SetLayerNames(names);
    ImGui::SameLine();
    ImGui::TextDisabled("(class + prop visual row)");
    bool net = OO::LayerNet();
    if (ImGui::Checkbox("Net identity", &net)) OO::SetLayerNet(net);
    ImGui::SameLine();
    ImGui::TextDisabled("(eid, mirror/local/untracked, key)");
    bool phys = OO::LayerPhys();
    if (ImGui::Checkbox("Physics state", &phys)) OO::SetLayerPhys(phys);
    ImGui::SameLine();
    ImGui::TextDisabled("(sim/rest + static/frozen/sleep)");
    float r = OO::RadiusM();
    ImGui::SetNextItemWidth(170.f);
    if (ImGui::SliderFloat("Radius (m)", &r, 5.f, 100.f, "%.0f")) OO::SetRadiusM(r);
    ImGui::EndDisabled();
    ImGui::Unindent(22.f);
}

void RenderSpawnNpc() {
    if (ImGui::Button("Spawn kerfurOmega (in front)")) coop::dev::spawn_npc::SpawnKerfurOmega();
    ImGui::SameLine();
    ImGui::TextDisabled("(host spawns + syncs)");
    // Increment-1 mirror test: spawn the new allowlist creatures directly (the F1
    // wisps/ventCrawler EVENTS don't reliably spawn a catchable one -- see spawn_npc).
    if (ImGui::Button("Spawn killerWisp (in front)"))  coop::dev::spawn_npc::SpawnKillerWisp();
    ImGui::SameLine();
    if (ImGui::Button("Spawn ventCrawler (in front)")) coop::dev::spawn_npc::SpawnVentCrawler();
    ImGui::SameLine();
    ImGui::TextDisabled("(mirror test: watch the client radar)");
    // v72 Killer Wisp coop cross-peer-kill test: spawn the wisp ON a client puppet so it
    // grabs the CLIENT (routes the kill there) instead of the nearest = the host.
    if (ImGui::Button("Spawn killerWisp ON client (coop kill test)"))
        coop::dev::spawn_npc::SpawnKillerWispOnClient();
}

void RenderGivePoints() {
    if (ImGui::Button("+1000 Points")) coop::dev::add_points::GivePoints(1000);
    ImGui::SameLine();
    ImGui::TextDisabled("(local balance -- afford drone orders)");
}

void RenderSpawnMenuUnlock() {
    namespace SM = coop::dev::spawn_menu_unlock;
    bool on = SM::IsEnabled();
    if (ImGui::Checkbox("Prop spawn menu in story mode (Q)", &on)) SM::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(host/local only)");
    ImGui::Indent(22.f);
    ImGui::TextDisabled("Enables the sandbox Q spawn menu in story mode.");
    if (ImGui::Button("Open spawn menu now")) SM::OpenNow();
    ImGui::SameLine();
    ImGui::TextDisabled("(or press Q while enabled)");
    ImGui::Unindent(22.f);
}

// Case-insensitive substring (the filter box; avoids imgui_internal.h).
bool StrContainsI(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && (tolower(static_cast<unsigned char>(*a)) ==
                            tolower(static_cast<unsigned char>(*b)))) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

void RenderEvents() {
    namespace ET = coop::dev::event_trigger;
    ImGui::TextDisabled("Trigger any game event (host only; the game's own runEvent path).");
    ImGui::TextDisabled("'day N' = unlock day (triggering ignores it). 'native trigger' = how the GAME");
    ImGui::TextDisabled("fires it (info only). 'random daily roll' = eligible from that day (native picker).");
    static char filter[32] = {};
    ImGui::SetNextItemWidth(180.f);
    ImGui::InputTextWithHint("##evfilter", "filter (name / category / trigger)...", filter, sizeof(filter));
    ImGui::Separator();
    // Horizontal scrollbar: the native-trigger column can run past the panel on a narrow window, so the
    // user can scroll right rather than lose the text (the window itself is also wider + min-constrained).
    ImGui::BeginChild("##evlist", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const char* lastCat = nullptr;
    for (const auto& ev : ET::Events()) {
        if (filter[0] && !StrContainsI(ev.name, filter) && !StrContainsI(ev.category, filter) &&
            !StrContainsI(ev.trigger, filter))
            continue;
        if (!lastCat || strcmp(lastCat, ev.category) != 0) {
            lastCat = ev.category;
            ImGui::SeparatorText(ev.category);
        }
        const bool danger = ev.risk == ET::Risk::Dangerous;
        if (danger)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        else if (ev.risk == ET::Risk::Caution)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
        const bool clicked = ImGui::Button(ev.name, ImVec2(150, 0));
        if (ev.risk != ET::Risk::Safe) ImGui::PopStyleColor();
        const bool hoveredBtn = ImGui::IsItemHovered();
        ImGui::SameLine();
        if (ev.dayZ >= 0) ImGui::TextDisabled("day %-3d", ev.dayZ);
        else              ImGui::TextDisabled("story  ");
        ImGui::SameLine();
        // The native trigger -- how the GAME fires this event, distinct from our F1 runEvent.
        ImGui::TextDisabled("%s", ev.trigger);
        if (hoveredBtn) {
            if (danger)
                ImGui::SetTooltip("native trigger: %s\n\nStory/save progression or relocation -- can "
                                  "desync the run.\nCtrl+click to trigger.", ev.trigger);
            else
                ImGui::SetTooltip("native trigger: %s", ev.trigger);
        }
        if (clicked && (!danger || ImGui::GetIO().KeyCtrl)) ET::Trigger(ev);
    }
    ImGui::EndChild();
}

// ---- the strict nested taxonomy (refined as features land) -------------------
// Player > Movement/Vitals/HUD ; Game > Weather/Entities/Events ; Network >
// Stats/Session ; Cosmetics > Skins. Network subs + Events + Cosmetics are still
// placeholders (future panels). Cosmetics is the one non-dev category (shown to
// all players).
const std::vector<Cat>& Tree() {
    static const std::vector<Cat> kTree = {
        { "Player", {
            { "Movement", { { &RenderTeleportClients, true }, { &RenderFreecam, true } }, true },
            { "Vitals",   { { &RenderRestoreVitals, true } }, true },
            { "HUD",      { { &RenderPosHud, true }, { &RenderObjectOverlay, true } }, true },
        }, true },
        { "Game", {
            { "Weather",  { { &RenderSnow, true } }, true },
            { "Entities", { { &RenderSpawnNpc, true }, { &RenderSpawnMenuUnlock, true } }, true },
            { "Economy",  { { &RenderGivePoints, true } }, true },
            { "Clock",    { { &RenderSetClock, true } }, true },
            { "Events",   { { &RenderEvents, true } }, true },
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
    // Dev items show only when the dev switch is on AND the master isn't killed --
    // [dev] enabled=0 forces every dev feature off (this replaces the per-module
    // MasterEnabled() checks the migrated dev modules used to do in their Init).
    g_devMode = ::coop::ini_config::MasterEnabled() &&
                ::coop::ini_config::IsIniKeyTrue("devkeys");
}

bool DevMode() {
    // The ini switch AND the live role gate: dev tools vanish the moment this
    // peer is a connected CLIENT (coop::dev_gate -- "strictly no dev on a
    // client"). Re-appear on disconnect / when hosting. Render-thread safe.
    return g_devMode && ::coop::dev_gate::Allowed();
}

void Render() {
    const bool devMode = DevMode();
    const auto& tree = Tree();

    // Wider default + a MIN-size constraint so the content panel never gets cramped (user: elements
    // didn't fit, had to widen every time). The min applies even when imgui.ini saved a narrow size,
    // so it self-corrects an already-too-narrow window on next open; the user can still grow it.
    ImGui::SetNextWindowSize(ImVec2(820, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 320), ImVec2(100000.0f, 100000.0f));
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
            if (g_devMode && !::coop::dev_gate::Allowed())
                ImGui::TextWrapped("Developer tools are disabled while connected as a "
                                   "client (host-only).");
            else
                ImGui::TextWrapped("Developer tools are hidden. Set [dev] devkeys=1 in "
                                   "votv-coop.ini to show them.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace ui::dev_menu
