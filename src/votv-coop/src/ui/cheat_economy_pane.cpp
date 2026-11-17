// ui/cheat_economy_pane.cpp -- F1 > Host tools > Economy (and, when the host allows it, Player tools > Economy).
//
// Add or remove points, the game's money: the current balance, preset steps, and a custom amount. A change is
// coop::cheats::AddPoints -- run at once by the host (the balance lane then mirrors it to every client), asked of the
// host by a client. A removal stops at zero.

#include "ui/cheat_panes.h"

#include "coop/cheats/balance_readout.h"
#include "coop/cheats/cheat_request.h"
#include "coop/dev/dev_gate.h"
#include "coop/net/protocol.h"
#include "l10n/l10n.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ui::cheat_panes {

void RenderEconomy() {
    using ui::scale::S;
    namespace CH = coop::cheats;
    CH::balance_readout::RequestRefresh();

    char line[128];
    int32_t balance = 0;
    if (CH::balance_readout::Latest(&balance))
        l10n::Fmt(line, sizeof(line), l10n::T("Points: %d"), balance);
    else
        std::snprintf(line, sizeof(line), "%s", l10n::T("Points: not available yet"));
    ImGui::TextUnformatted(line);

    ImGui::Spacing();
    ImGui::SeparatorText(l10n::Label(l10n::T("Add points"), "add_points"));
    static const int kSteps[] = {100, 1000, 10000, 100000};
    const bool canAsk = CH::CanAsk();   // a client's menu offers nothing the host would drop
    ImGui::BeginDisabled(!canAsk);
    for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i) {
        if (i) ImGui::SameLine();
        std::snprintf(line, sizeof(line), "+%d###add%d", kSteps[i], kSteps[i]);
        if (ImGui::Button(line)) CH::AddPoints(kSteps[i]);
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText(l10n::Label(l10n::T("Remove points"), "remove_points"));
    ImGui::BeginDisabled(!canAsk);
    for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i) {
        if (i) ImGui::SameLine();
        std::snprintf(line, sizeof(line), "-%d###sub%d", kSteps[i], kSteps[i]);
        if (ImGui::Button(line)) CH::AddPoints(-kSteps[i]);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(l10n::Label(l10n::T("Custom amount"), "custom_amount"));
    static int amount = 500;
    const int limit = coop::dev_gate::Allowed() ? 1000000000 : coop::net::kCheatMaxPoints;
    ImGui::SetNextItemWidth(S(160.f));
    ImGui::InputInt(l10n::Label(l10n::T("Amount"), "amount"), &amount, 100, 1000);
    if (amount < 1) amount = 1;
    if (amount > limit) amount = limit;
    ImGui::BeginDisabled(!canAsk);
    if (ImGui::Button(l10n::Label(l10n::T("Add"), "add_custom"))) CH::AddPoints(amount);
    ImGui::SameLine();
    if (ImGui::Button(l10n::Label(l10n::T("Remove"), "remove_custom"))) CH::AddPoints(-amount);
    ImGui::EndDisabled();

    const std::string status = CH::StatusLine(CH::LastStatus());
    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", status.c_str());
    }
}

}  // namespace ui::cheat_panes
