// ui/cheat_entities_pane.cpp -- F1 > Host tools > Entities: the creatures the host can spawn.
//
// Each button spawns one of the game's own creatures beside the host, through coop/dev/spawn_npc and
// coop/dev/mannequin_drill; the host's creature lanes then show it to every client. Host only.

#include "ui/cheat_panes.h"

#include "coop/dev/dev_gate.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/dev/spawn_npc.h"
#include "l10n/l10n.h"

#include "imgui.h"

#include <cstdio>

namespace ui::cheat_panes {
namespace {

namespace SN = coop::dev::spawn_npc;

// A spawn button named for the creature, with the note that says what to expect.
void Spawn(const char* creature, const char* id, void (*fn)(), const char* note) {
    char text[128];
    l10n::Fmt(text, sizeof(text), l10n::T("Spawn %s in front of me"), creature);
    if (ImGui::Button(l10n::Label(text, id))) fn();
    if (note) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", note);
    }
}

}  // namespace

void RenderEntities() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", l10n::T("Only the host can do this."));
        return;
    }
    ImGui::TextDisabled("%s", l10n::T("The creatures are the game's own; every player sees them."));
    ImGui::Spacing();
    Spawn("kerfurOmega", "spawn_kerfur", &SN::SpawnKerfurOmega, nullptr);
    Spawn("killerWisp", "spawn_wisp", &SN::SpawnKillerWisp, nullptr);
    Spawn("ventCrawler", "spawn_crawler", &SN::SpawnVentCrawler, nullptr);
    Spawn("geomOcta", "spawn_octa", &SN::SpawnGeomOcta, l10n::T("(its beam and blast run on the host)"));
    Spawn("Eyer", "spawn_eyer", &SN::SpawnEyer,
          l10n::T("(it stalks you; the other players see a harmless copy)"));
    Spawn(l10n::T("a walking mannequin"), "spawn_mannequin", &coop::dev::mannequin_drill::SpawnWalker,
          l10n::T("(the game's own; use a throwaway save)"));
    ImGui::Spacing();
    if (ImGui::Button(l10n::Label(l10n::T("Spawn killerWisp on a client"), "spawn_wisp_on_client")))
        SN::SpawnKillerWispOnClient();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", l10n::T("(it goes for that player, not for you)"));
}

}  // namespace ui::cheat_panes
