// ui/cheat_entities_pane.cpp -- F1 > Host tools > Entities: the creatures the host can spawn.
//
// Each button spawns one of the game's own creatures beside the host through coop/dev/spawn_npc; a developer tool may
// add an entry of its own (ui/cheat_entities). The host's creature lanes then show each to every client. Host only.

#include "ui/cheat_entities.h"
#include "ui/cheat_panes.h"

#include "coop/dev/dev_gate.h"
#include "coop/dev/spawn_npc.h"
#include "l10n/l10n.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ui::cheat_entities {
namespace {

struct DevEntry { std::string label; void (*spawn)(); };
std::mutex g_devMutex;
std::vector<DevEntry> g_dev;   // guarded by g_devMutex

}  // namespace

void AddDevEntry(const char* label, void (*spawn)()) {
    if (!label || !spawn) return;
    std::lock_guard<std::mutex> lock(g_devMutex);
    for (const DevEntry& e : g_dev)
        if (e.label == label) return;
    g_dev.push_back({label, spawn});
}

namespace detail {

// A copy of the entries, taken under the lock: the pane draws without holding it.
std::vector<std::pair<std::string, void (*)()>> DevEntries() {
    std::vector<std::pair<std::string, void (*)()>> out;
    std::lock_guard<std::mutex> lock(g_devMutex);
    for (const DevEntry& e : g_dev) out.emplace_back(e.label, e.spawn);
    return out;
}

}  // namespace detail

}  // namespace ui::cheat_entities

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
    for (const auto& e : ui::cheat_entities::detail::DevEntries()) {
        const std::string id = "spawn_dev_" + e.first;
        Spawn(e.first.c_str(), id.c_str(), e.second, nullptr);
    }
    ImGui::Spacing();
    if (ImGui::Button(l10n::Label(l10n::T("Spawn killerWisp on a client"), "spawn_wisp_on_client")))
        SN::SpawnKillerWispOnClient();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", l10n::T("(it goes for that player, not for you)"));
}

}  // namespace ui::cheat_panes
