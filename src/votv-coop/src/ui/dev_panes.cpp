// ui/dev_panes.cpp -- see ui/dev_panes.h.

#include "ui/dev_panes.h"

#include "coop/dev/dev_gate.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/dev/freecam.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/pos_hud.h"
#include "coop/dev/add_points.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/set_clock.h"
#include "coop/dev/spawn_menu_unlock.h"
#include "coop/dev/spawn_npc.h"
#include "coop/permissions/grants_core.h"
#include "coop/session/local_grants.h"
#include "coop/session/teleport_client.h"
#include "ui/scale.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

namespace ui::dev_panes {

using ui::scale::S;

// ---- feature controls (each calls a plain function its module exposes) -------

// A world pane writes the shared world: host authority (coop/dev/dev_gate). A local pane touches only
// this machine: the host's grant (coop/session/local_grants).
constexpr const char* kHostOnlyLine = "Only the host can do this.";
constexpr const char* kNotAllowedLine = "The host has not allowed this.";

void RenderSnow() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    bool on = coop::dev::force_weather::IsSnowOn();
    // Host-authoritative: the toggle is a no-op on a client (the module gates it).
    if (ImGui::Checkbox("Snow", &on)) coop::dev::force_weather::SetSnow(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(host-authoritative; clients mirror)");
}

void RenderTeleportClients() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    if (ImGui::Button("Teleport clients to me")) coop::teleport_client::TeleportClientsToHost();
    ImGui::SameLine();
    ImGui::TextDisabled("(host only)");
}

void RenderFreecam() {
    if (!coop::session::local_grants::Has(coop::permissions::grants::Projected::Freecam)) {
        ImGui::TextDisabled("%s", kNotAllowedLine);
        return;
    }
    bool on = coop::dev::freecam::IsActive();
    if (ImGui::Checkbox("Freecam", &on)) coop::dev::freecam::SetActive(on);
    ImGui::TextDisabled("HOME also toggles - WASD move, Space/Ctrl up/down,");
    ImGui::TextDisabled("Shift fast, wheel speed");
    // The middle-click self-teleport moves the pawn in the shared world: the host's alone.
    if (coop::dev_gate::Allowed()) ImGui::TextDisabled("MMB bring player");
}

void RenderRestoreVitals() {
    if (coop::dev_gate::Allowed()) {
        if (ImGui::Button("Restore vitals (food / sleep / health)")) coop::dev::restore_vitals::Restore();
        ImGui::SameLine();
        ImGui::TextDisabled("(both peers)");
    } else {
        ImGui::TextDisabled("%s", kHostOnlyLine);
    }
    if (coop::session::local_grants::Has(coop::permissions::grants::Projected::Stamina)) {
        if (ImGui::Button("Set stamina low")) coop::dev::restore_vitals::SetStaminaLow();
        ImGui::SameLine();
        ImGui::TextDisabled("(local test -- sleep/energy=10 -> exhausted; nameplate syncs)");
    } else {
        ImGui::TextDisabled("%s", kNotAllowedLine);
    }
}

void RenderSetClock() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    namespace SC = coop::dev::set_clock;
    int hour = 0, minute = 0, day = 0;
    float frac = 0.f;
    if (!SC::ReadCurrent(hour, minute, day, frac)) {
        ImGui::TextDisabled("World clock not resolved yet (enter a world).");
        return;
    }
    ImGui::TextDisabled("Host-authoritative; the clock (timeZ -> settime) drives events + the save.");
    ImGui::Text("Now: Day %d — %02d:%02d", day, hour, minute);
    ImGui::SameLine();
    ImGui::TextDisabled("(sun %.3f)", frac);
    static int s_day = -1, s_hour = -1, s_min = -1;
    if (s_day < 0) { s_day = day; s_hour = hour; s_min = minute; }  // seed once from the live clock
    ImGui::SetNextItemWidth(S(90.f));
    ImGui::InputInt("day##clk", &s_day);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(74.f));
    ImGui::InputInt("h##clk", &s_hour);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(74.f));
    ImGui::InputInt("m##clk", &s_min);
    if (s_day < 1) s_day = 1;
    if (s_hour < 0) s_hour = 0;
    if (s_hour > 23) s_hour = 23;
    if (s_min < 0) s_min = 0;
    if (s_min > 59) s_min = 59;
    ImGui::SameLine();
    if (ImGui::Button("Set clock")) SC::SetClock(s_day, s_hour, s_min);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the game's own running clock (timeZ); the next minute pulse runs\n"
                          "saveSlot.settime natively. Jumping the day FORWARD fires every skipped\n"
                          "scheduled story event at once (native settime behavior; they mirror to\n"
                          "clients via EventFire). Day is the DISPLAYED day (as on the save rows).");
    ImGui::SameLine();
    ImGui::TextDisabled("(host only)");
    float f = frac;
    ImGui::SetNextItemWidth(S(260.f));
    if (ImGui::SliderFloat("Sun position", &f, 0.0f, 0.999f, "%.3f"))
        SC::SetTimeFraction(f);  // live: drag moves the sun (totalTime := frac * MaxTime)
    ImGui::TextDisabled("Visual only (sun angle). The clock above is what events/save follow.");
}

void RenderPosHud() {
    if (!coop::session::local_grants::Has(coop::permissions::grants::Projected::Hud)) {
        ImGui::TextDisabled("%s", kNotAllowedLine);
        return;
    }
    bool on = coop::dev::pos_hud::IsVisible();
    if (ImGui::Checkbox("Position / camera readout", &on)) coop::dev::pos_hud::SetVisible(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(on-screen overlay)");
}

void RenderObjectOverlay() {
    if (!coop::session::local_grants::Has(coop::permissions::grants::Projected::Overlay)) {
        ImGui::TextDisabled("%s", kNotAllowedLine);
        return;
    }
    namespace OO = coop::dev::object_overlay;
    bool on = OO::IsEnabled();
    if (ImGui::Checkbox("World object overlay", &on)) OO::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(labels projected onto nearby objects)");
    ImGui::Indent(S(22.f));
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
    bool hp = OO::LayerHealth();
    if (ImGui::Checkbox("Health / progress", &hp)) OO::SetLayerHealth(hp);
    ImGui::SameLine();
    ImGui::TextDisabled("(creature+prop hp; decal clean/cement pools)");
    float r = OO::RadiusM();
    ImGui::SetNextItemWidth(S(170.f));
    if (ImGui::SliderFloat("Radius (m)", &r, 5.f, 100.f, "%.0f")) OO::SetRadiusM(r);
    ImGui::EndDisabled();
    ImGui::Unindent(S(22.f));
}

void RenderRagdollBones() {
    if (!coop::session::local_grants::Has(coop::permissions::grants::Projected::Overlay)) {
        ImGui::TextDisabled("%s", kNotAllowedLine);
        return;
    }
    namespace RB = coop::dev::ragdoll_bone_overlay;
    bool on = RB::IsEnabled();
    if (ImGui::Checkbox("Ragdoll bone skeleton", &on)) RB::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(lines between every bone of an ACTIVE ragdoll body)");
    ImGui::TextDisabled("The native ragdoll (C key / faint / trip) is an invisible separate actor;");
    ImGui::TextDisabled("orange = your own ragdoll, cyan = a remote peer's mirror body.");
}

void RenderSpawnNpc() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    if (ImGui::Button("Spawn kerfurOmega (in front)")) coop::dev::spawn_npc::SpawnKerfurOmega();
    ImGui::SameLine();
    ImGui::TextDisabled("(host spawns + syncs)");
    // Spawn the allowlisted creatures directly, in front of the player: the wisp event arms an
    // overlap box, and the eventer's ventCrawler lands about ten metres away in a vent (see
    // spawn_npc).
    if (ImGui::Button("Spawn killerWisp (in front)"))  coop::dev::spawn_npc::SpawnKillerWisp();
    ImGui::SameLine();
    if (ImGui::Button("Spawn ventCrawler (in front)")) coop::dev::spawn_npc::SpawnVentCrawler();
    ImGui::SameLine();
    ImGui::TextDisabled("(mirror test: watch the client radar)");
    // Killer-wisp cross-peer kill test: spawn the wisp ON a client puppet so it grabs the
    // CLIENT, routing the kill there, instead of the nearest, which is the host.
    if (ImGui::Button("Spawn killerWisp ON client (coop kill test)"))
        coop::dev::spawn_npc::SpawnKillerWispOnClient();
    ImGui::Separator();
    // Owner-entity lane test: an eyer is per-peer OWNED, its native AI stalking the spawning
    // peer, and cross-peer VISIBLE, other peers getting a brain-parked display mirror.
    if (ImGui::Button("Spawn Eyer (in front)")) coop::dev::spawn_npc::SpawnEyer();
    ImGui::SameLine();
    ImGui::TextDisabled("(owner-entity test: it stalks YOU; peers see a harmless mirror)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The night 'Eyes' stalker. Each peer keeps its OWN native eyer\n"
                          "(it targets the peer that rolled it); every other peer renders a\n"
                          "display mirror -- AI and killsphere disabled on the mirror.");
    if (ImGui::Button("Spawn walking mannequin")) coop::dev::mannequin_drill::SpawnWalker();
    ImGui::SameLine();
    ImGui::TextDisabled("(the game's own creature: use a throwaway save)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game's walking mannequin, spawned at its nearest spawn point that is not on screen "
                          "(it normally appears from day 14).\n"
                          "It opens doors, is saved with the world and returns angry after a load.\n"
                          "In a session it crosses to each client as a prop mirror that runs a brain of its own.");
}

void RenderGivePoints() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    if (ImGui::Button("+1000 Points")) coop::dev::add_points::GivePoints(1000);
    ImGui::SameLine();
    ImGui::TextDisabled("(local balance -- afford drone orders)");
}

void RenderSpawnMenuUnlock() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    namespace SM = coop::dev::spawn_menu_unlock;
    bool on = SM::IsEnabled();
    if (ImGui::Checkbox("Prop spawn menu in story mode (Q)", &on)) SM::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(host/local only)");
    ImGui::Indent(S(22.f));
    ImGui::TextDisabled("Enables the sandbox Q spawn menu in story mode.");
    if (ImGui::Button("Open spawn menu now")) SM::OpenNow();
    ImGui::SameLine();
    ImGui::TextDisabled("(or press Q while enabled)");
    ImGui::Unindent(S(22.f));
}

// Case-insensitive substring (the filter box; avoids imgui_internal.h).
static bool StrContainsI(const char* hay, const char* needle) {
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
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", kHostOnlyLine);
        return;
    }
    namespace ET = coop::dev::event_trigger;
    namespace EF = coop::dev::event_force;
    EF::RequestRefresh();  // ~1 Hz internal limiter; keeps the ARMED badges live while the tab is open
    ImGui::TextDisabled("Trigger any game event (host only; runEvent + runSpecialEvent + ambient verbs).");
    ImGui::TextDisabled("Grouped by strict");
    ImGui::TextDisabled("category. 'day N' = unlock day. The cyan TIME column is the native trigger time-of-");
    ImGui::TextDisabled("day (HH:MM anchor) / 'trigger' (story/build) / 'rep' (ariral-reputation prank pool).");
    ImGui::TextDisabled("Volume-gated events show a badge: fire only ARMS a level volume; NOW! completes it");
    ImGui::TextDisabled("instantly (drives the volume's own overlap with your pawn -- the native walk-in).");
    static char filter[32] = {};
    ImGui::SetNextItemWidth(S(180.f));
    ImGui::InputTextWithHint("##evfilter", "filter (name / category / time / effect)...", filter, sizeof(filter));
    ImGui::Separator();
    // Horizontal scrollbar: the effect column can run past the panel on a narrow window, so the
    // reader scrolls right rather than losing the text. The window itself is wider and
    // min-constrained too.
    ImGui::BeginChild("##evlist", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ET::Category lastCat = ET::Category::COUNT;
    bool haveCat = false;
    for (const auto& ev : ET::Events()) {
        const char* catName = ET::CategoryName(ev.cat);
        if (filter[0] && !StrContainsI(ev.name, filter) && !StrContainsI(catName, filter) &&
            !StrContainsI(ev.time, filter) && !StrContainsI(ev.mechanism, filter))
            continue;
        if (!haveCat || ev.cat != lastCat) {
            lastCat = ev.cat;
            haveCat = true;
            ImGui::SeparatorText(catName);
        }
        const bool danger = ev.risk == ET::Risk::Dangerous;
        if (danger)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        else if (ev.risk == ET::Risk::Caution)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
        const bool clicked = ImGui::Button(ev.name, ImVec2(S(150.f), 0));
        if (ev.risk != ET::Risk::Safe) ImGui::PopStyleColor();
        const bool hoveredBtn = ImGui::IsItemHovered();
        ImGui::SameLine();
        if (ev.dayZ >= 0) ImGui::TextDisabled("day %-3d", ev.dayZ);
        else              ImGui::TextDisabled("  -   ");
        ImGui::SameLine();
        // The native trigger TIME (hours:minutes) -- the user's key ask. Cyan so it stands out.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.95f, 1.0f));
        ImGui::Text("%-7s", ev.time);
        ImGui::PopStyleColor();
        // Volume-gated events: the live gate badge + the force-NOW button.
        const EF::BoxStatus box = EF::StatusFor(ev.name);
        bool forceClicked = false, forceHovered = false;
        if (box.hasBox) {
            ImGui::SameLine();
            if (!box.resolved)
                ImGui::TextDisabled("[gate: volume ?]");
            else if (box.shots == 0)
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "[FIRED]");
            else if (box.armed)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[ARMED - walk-in pending]");
            else
                ImGui::TextDisabled("[volume-gated]");
            ImGui::SameLine();
            ImGui::PushID(ev.name);
            forceClicked = ImGui::SmallButton("NOW!");
            forceHovered = ImGui::IsItemHovered();
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ev.mechanism);
        const char* note = box.hasBox ? nullptr : EF::GateNote(ev.name);
        if (forceHovered) {
            ImGui::SetTooltip("Arm + complete NOW: fires the event (clients get the arm as usual), then\n"
                              "drives %s's own overlap handler with your pawn -- the same dispatch a real\n"
                              "walk-in performs (native class filter + N bookkeeping).%s",
                              box.boxName, danger ? "\n\nDangerous event: Ctrl+click." : "");
        } else if (hoveredBtn) {
            if (danger) {
                ImGui::SetTooltip("%s  |  native time: %s\n%s%s%s%s\n\nStory/save progression or player relocation"
                                  " --\ncan desync the run. Ctrl+click to trigger.", catName, ev.time, ev.mechanism,
                                  (box.hasBox || note) ? "\ngate: " : "",
                                  box.hasBox ? "fire ARMS the level volume " : (note ? note : ""),
                                  box.hasBox ? box.boxName : "");
            } else {
                const char* path = (ev.dispatch == ET::Dispatch::SpecialEvent) ? "runSpecialEvent (specific prank)"
                                 : (ev.dispatch == ET::Dispatch::RandomPrank)  ? "runEvent (RANDOM rep-tier prank)"
                                 : (ev.dispatch == ET::Dispatch::Ambient)      ? "direct ambient UFunction (daynight/gamemode)"
                                 :                                               "runEvent";
                ImGui::SetTooltip("%s  |  native time: %s  |  via %s\n%s%s%s%s", catName, ev.time, path, ev.mechanism,
                                  (box.hasBox || note) ? "\ngate: " : "",
                                  box.hasBox ? "fire ARMS the level volume; walk into it (or NOW!) to complete: " : (note ? note : ""),
                                  box.hasBox ? box.boxName : "");
            }
        }
        if (clicked && (!danger || ImGui::GetIO().KeyCtrl)) ET::Trigger(ev);
        if (forceClicked && (!danger || ImGui::GetIO().KeyCtrl)) EF::ForceNow(ev.name);
    }
    ImGui::EndChild();
}

}  // namespace ui::dev_panes
