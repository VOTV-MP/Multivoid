// ui/dev_panes.cpp -- see ui/dev_panes.h.

#include "ui/dev_panes.h"

#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/restore_vitals.h"
#include "coop/permissions/grants_core.h"
#include "coop/session/local_grants.h"
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

// A local pane touches only this machine: the host's grant (coop/session/local_grants). The panes that write the
// shared world (items, points, events, weather, the clock, creatures) are the player-facing Host tools
// (ui/cheat_panes), translated.
constexpr const char* kNotAllowedLine = "The host has not allowed this.";

void RenderStaminaLow() {
    if (coop::session::local_grants::Has(coop::permissions::grants::Projected::Stamina)) {
        if (ImGui::Button("Set stamina low")) coop::dev::restore_vitals::SetStaminaLow();
        ImGui::SameLine();
        ImGui::TextDisabled("(local test -- sleep/energy=10 -> exhausted; nameplate syncs)");
    } else {
        ImGui::TextDisabled("%s", kNotAllowedLine);
    }
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

}  // namespace ui::dev_panes
