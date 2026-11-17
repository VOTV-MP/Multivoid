// ui/player_tools_pane.cpp -- F1 > Player tools > Myself (and Host tools > Myself): a player's tools for its own body.
//
// A client's own conveniences: teleport to the host, unstuck, and the local tools the host has granted (the free
// camera, the position readout). None of it writes the shared world: a body is its own machine's to place
// (coop/cheats/self_teleport) and a granted tool is local to this machine (coop/session/local_grants). The pane also
// says whether the host lets players use the cheat tools, the Items, Economy and Events beside it.

#include "ui/cheat_panes.h"

#include "coop/cheats/cheat_request.h"
#include "coop/cheats/self_teleport.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/freecam.h"
#include "coop/dev/pos_hud.h"
#include "coop/permissions/grants_core.h"
#include "coop/session/local_grants.h"
#include "l10n/l10n.h"

#include "imgui.h"

namespace ui::cheat_panes {

void RenderMyself() {
    namespace G = coop::permissions::grants;
    namespace LG = coop::session::local_grants;
    const bool client = !coop::dev_gate::Allowed();

    ImGui::SeparatorText(l10n::Label(l10n::T("Move"), "myself_move"));
    // Teleport to the host is a cheat: offered while the host allows players cheats and has not withheld this player's
    // right. The gate is this menu's: the move is this machine's own (a modified client could make it anyway), and the
    // host's body is not moved by it.
    const bool mayCheat = client && CheatFrameNow().mayCheat;
    if (mayCheat) {
        if (ImGui::Button(l10n::Label(l10n::T("Teleport to the host"), "tp_to_host"))) coop::cheats::self_teleport::ToPlayer(0);
        ImGui::SameLine();
    }
    if (ImGui::Button(l10n::Label(l10n::T("Unstuck"), "unstuck"))) coop::cheats::self_teleport::Unstuck();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", l10n::T("(Unstuck puts you on the nearest safe floor)"));

    ImGui::SeparatorText(l10n::Label(l10n::T("Tools the host grants"), "myself_grants"));
    if (LG::Has(G::Projected::Freecam)) {
        bool on = coop::dev::freecam::IsActive();
        if (ImGui::Checkbox(l10n::Label(l10n::T("Free camera"), "freecam"), &on)) coop::dev::freecam::SetActive(on);
        ImGui::TextDisabled("%s", l10n::T("HOME also toggles it. WASD move, Space and Ctrl up and down, Shift fast, wheel "
                                          "changes the speed."));
    } else {
        ImGui::TextDisabled("%s", l10n::T("Free camera: the host has not allowed this."));
    }
    if (LG::Has(G::Projected::Hud)) {
        bool on = coop::dev::pos_hud::IsVisible();
        if (ImGui::Checkbox(l10n::Label(l10n::T("Position and camera readout"), "pos_hud"), &on))
            coop::dev::pos_hud::SetVisible(on);
    } else {
        ImGui::TextDisabled("%s", l10n::T("Position readout: the host has not allowed this."));
    }

    if (client) {
        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Cheats"), "myself_cheats"));
        if (mayCheat)
            ImGui::TextUnformatted(l10n::T("The host lets players use cheats: Items, Economy and Events are on the left. "
                                           "Each use is a request the host checks and runs."));
        else if (CheatFrameNow().hostAllows)
            ImGui::TextDisabled("%s", l10n::T("The host has not given you the cheat tools."));
        else
            ImGui::TextDisabled("%s", l10n::T("The host has not allowed cheats."));
    }
}

}  // namespace ui::cheat_panes
