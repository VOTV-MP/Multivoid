// ui/cheat_access_pane.cpp -- F1 > Host tools > Player cheats: the host's switch for its players' cheat tools.
//
// One checkbox, the server setting net.allow_player_cheats ("Let players use cheats", off by default). It is a
// replicated server row like the others the Administration pane draws, so it changes the same way a typed /set does:
// the pane submits the `set` line, the host's setter stores it (it persists with the host's other settings) and every
// client learns the value. While it is on the players' menu offers Items, Economy and Events; each use is a request the
// host checks and runs.

#include "ui/cheat_panes.h"

#include "coop/commands/command_sync.h"
#include "coop/config/config_registry.h"
#include "l10n/l10n.h"
#include "ui/scale.h"

#include "imgui.h"

#include <string>

namespace ui::cheat_panes {

namespace {
CheatFrame g_frame;   // render thread
}  // namespace

void SetCheatFrame(const CheatFrame& f) { g_frame = f; }
const CheatFrame& CheatFrameNow() { return g_frame; }

void RenderPlayerCheats() {
    const auto& row = *coop::config_registry::rows::net_allow_player_cheats.row;
    bool on = g_frame.hostAllows;
    if (ImGui::Checkbox(l10n::Label(l10n::T("Let players use cheats"), "let_players_cheat"), &on))
        coop::command_sync::Submit(std::string("set ") + row.key + (on ? " 1" : " 0"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ui::scale::S(520.f));
    ImGui::TextUnformatted(l10n::T(row.desc));
    ImGui::Spacing();
    if (on)
        ImGui::TextUnformatted(l10n::T("Your players now see Items, Economy and Events under Player tools. Each use is "
                                       "a request you check and run: it is logged on your machine and shown in your "
                                       "chat. Players are offered only the safe events and no explosives; story events "
                                       "and anything that moves players stay yours alone."));
    else
        ImGui::TextUnformatted(l10n::T("Players see only their own tools (teleport to you, unstuck, what you grant)."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", l10n::T("The setting is saved with your other server settings and is also under "
                                      "Administration > Server settings."));
}

}  // namespace ui::cheat_panes
