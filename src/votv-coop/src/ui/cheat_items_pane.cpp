// ui/cheat_items_pane.cpp -- F1 > Host tools > Items (and, when the host allows it, Player tools > Items).
//
// Every prop the game can spawn -- the game's own list, read by coop/cheats/item_catalog -- as a searchable list with a
// count and two ways to place the props: in front of the player who asks, or at any player in the session. The spawn is
// coop::cheats::SpawnItem: run at once by the host, asked of the host by a client. The host also gets the game's own
// Q spawn menu here, enabled for story mode.

#include "ui/cheat_panes.h"

#include "coop/cheats/cheat_request.h"
#include "coop/cheats/cheat_run.h"
#include "coop/cheats/item_catalog.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/spawn_menu_unlock.h"
#include "coop/net/protocol.h"
#include "l10n/l10n.h"
#include "ui/cheat_player_picker.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ui::cheat_panes {
namespace {

using ui::scale::S;
namespace CH = coop::cheats;

// Case-insensitive substring.
bool ContainsI(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == n) return true;
    }
    return false;
}

// The names the filter keeps, rebuilt only when the filter or the list changes.
struct Filtered {
    // Held, not borrowed: a list freed under a raw pointer can be reallocated at its address.
    std::shared_ptr<const std::vector<std::string>> source;
    std::string filter;
    bool authority = false;   // the list was cut for a player (false) or is the host's whole list (true)
    size_t offered = 0;       // the names this pane offers, before the filter
    std::vector<int> index;
};

}  // namespace

void RenderItems() {
    const bool authority = coop::dev_gate::Allowed();
    CH::item_catalog::RequestRefresh();
    const auto names = CH::item_catalog::Names();
    if (!names) {
        ImGui::TextDisabled("%s", l10n::T("The game's item list is still loading -- it fills a little after the world is up."));
        return;
    }

    static char filterBuf[48] = {};
    static std::string selected;
    static int count = 1;
    static int target = -1;
    static Filtered cache;

    char line[160];
    ImGui::SetNextItemWidth(S(220.f));
    ImGui::InputTextWithHint("##itemfilter", l10n::T("Search items..."), filterBuf, sizeof(filterBuf));
    if (cache.source != names || cache.filter != filterBuf || cache.authority != authority) {
        cache.source = names;
        cache.filter = filterBuf;
        cache.authority = authority;
        cache.offered = 0;
        cache.index.clear();
        for (size_t i = 0; i < names->size(); ++i) {
            // A player is offered what the host's judge accepts: an explosive is left out of the list.
            if (!authority && !CH::ItemOpenToPlayers((*names)[i])) continue;
            ++cache.offered;
            if (ContainsI((*names)[i], filterBuf)) cache.index.push_back(static_cast<int>(i));
        }
    }
    ImGui::SameLine();
    if (filterBuf[0])
        l10n::Fmt(line, sizeof(line), l10n::T("%1$d of %2$d items"), static_cast<int>(cache.index.size()),
                  static_cast<int>(cache.offered));
    else
        l10n::Fmt(line, sizeof(line), l10n::T("%d items"), static_cast<int>(cache.offered));
    ImGui::TextDisabled("%s", line);

    const int maxCount = authority ? 50 : coop::net::kCheatMaxCount;
    if (count > maxCount) count = maxCount;
    ImGui::SetNextItemWidth(S(160.f));
    ImGui::SliderInt(l10n::Label(l10n::T("Count"), "count"), &count, 1, maxCount);

    if (selected.empty()) {
        ImGui::TextDisabled("%s", l10n::T("Pick an item from the list."));
    } else {
        l10n::Fmt(line, sizeof(line), l10n::T("Selected: %s"), selected.c_str());
        ImGui::TextUnformatted(line);
    }

    const bool canAsk = CH::CanAsk(count);   // a client's menu offers nothing the host would drop
    ImGui::BeginDisabled(selected.empty() || !canAsk);
    if (ImGui::Button(l10n::Label(l10n::T("Spawn in front of me"), "spawn_front")))
        CH::SpawnItem(selected, count, coop::net::kCheatInFront);
    if (ui::cheat_picker::Count() > 1) {
        ImGui::SameLine();
        char nick[96] = "";
        ui::cheat_picker::Choose(&target, true);
        ui::cheat_picker::NickOf(target, nick, sizeof(nick));
        l10n::Fmt(line, sizeof(line), l10n::T("Spawn at %s"), nick);
        if (ImGui::Button(l10n::Label(line, "spawn_at")) && target >= 0) CH::SpawnItem(selected, count, target);
        ImGui::SameLine();
        ui::cheat_picker::Combo("##spawntarget", &target, true);
    }
    ImGui::EndDisabled();

    const std::string status = CH::StatusLine(CH::LastStatus());
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

    if (authority) {
        ImGui::Spacing();
        namespace SM = coop::dev::spawn_menu_unlock;
        bool on = SM::IsEnabled();
        if (ImGui::Checkbox(l10n::Label(l10n::T("The game's spawn menu (Q) in story mode"), "spawn_menu_q"), &on))
            SM::SetEnabled(on);
        ImGui::SameLine();
        // One button, two states: what opened the menu also closes it.
        if (SM::IsMenuOpen()) {
            if (ImGui::SmallButton(l10n::Label(l10n::T("Close it"), "spawn_menu_open"))) SM::CloseNow();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", l10n::T("(Q closes it too)"));
        } else if (ImGui::SmallButton(l10n::Label(l10n::T("Open it now"), "spawn_menu_open"))) {
            SM::OpenNow();
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("##itemlist", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(cache.index.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::string& name = (*names)[static_cast<size_t>(cache.index[static_cast<size_t>(row)])];
            ImGui::PushID(row);
            const bool isSel = name == selected;
            if (ImGui::Selectable(name.c_str(), isSel, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = name;
                if (ImGui::IsMouseDoubleClicked(0) && canAsk) CH::SpawnItem(selected, count, coop::net::kCheatInFront);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

}  // namespace ui::cheat_panes
