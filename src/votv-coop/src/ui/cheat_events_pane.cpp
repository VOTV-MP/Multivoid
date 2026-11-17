// ui/cheat_events_pane.cpp -- F1 > Host tools > Events (and, when the host allows it, Player tools > Events).
//
// The game's events, grouped by kind, each a button that triggers it: the table coop/dev/event_trigger keeps from the
// game's own event list. A trigger is coop::cheats::FireEvent -- run at once by the host (its watch broadcasts the fire
// and every client replays it), asked of the host by a client. A player is offered only the events that do not move
// players or advance the story. On the host, an event that arms a level volume also gets a NOW! button that completes
// it, and a dangerous event asks for Ctrl+click.

#include "ui/cheat_panes.h"

#include "coop/cheats/cheat_request.h"
#include "coop/cheats/cheat_run.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "l10n/l10n.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cctype>
#include <cstring>
#include <string>

namespace ui::cheat_panes {
namespace {

using ui::scale::S;
namespace ET = coop::dev::event_trigger;
namespace EF = coop::dev::event_force;
namespace CH = coop::cheats;

bool ContainsI(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// The native trigger time as the pane shows it: an HH:MM anchor as it is, the table's words translated.
const char* TimeText(const char* t) {
    if (std::strcmp(t, "trigger") == 0) return l10n::T("trigger");
    if (std::strcmp(t, "rep-gated") == 0) return l10n::T("rep-gated");
    if (std::strcmp(t, "ambient") == 0) return l10n::T("ambient");
    return t;
}

}  // namespace

void RenderEvents() {
    const bool authority = coop::dev_gate::Allowed();
    const bool canAsk = CH::CanAsk();   // a client's menu offers nothing the host would drop
    if (authority) EF::RequestRefresh();   // keeps the ARMED badges live while the pane is open
    ImGui::TextDisabled("%s", l10n::T("Trigger a game event. 'day N' is the day it unlocks; the blue time is when the game "
                                      "schedules it."));
    if (authority)
        ImGui::TextDisabled("%s", l10n::T("An event with a level volume only arms it: walk in, or press NOW! to complete it. "
                                          "Red events can wreck a save: Ctrl+click."));
    static char filter[32] = {};
    ImGui::SetNextItemWidth(S(180.f));
    ImGui::InputTextWithHint("##evfilter", l10n::T("Search events..."), filter, sizeof(filter));
    const std::string status = CH::StatusLine(CH::LastStatus());
    if (!status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("##evlist", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ET::Category lastCat = ET::Category::COUNT;
    bool haveCat = false;
    for (const auto& ev : ET::Events()) {
        const bool danger = ev.risk == ET::Risk::Dangerous;
        if (!authority && !CH::OpenToPlayers(ev)) continue;
        const char* catName = l10n::T(ET::CategoryName(ev.cat));
        const char* mech = l10n::T(ev.mechanism);
        if (filter[0] && !ContainsI(ev.name, filter) && !ContainsI(catName, filter) && !ContainsI(ev.time, filter) &&
            !ContainsI(mech, filter))
            continue;
        if (!haveCat || ev.cat != lastCat) {
            lastCat = ev.cat;
            haveCat = true;
            ImGui::SeparatorText(catName);
        }
        if (danger)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        else if (ev.risk == ET::Risk::Caution)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
        ImGui::BeginDisabled(!canAsk);
        const bool clicked = ImGui::Button(ev.name, ImVec2(S(150.f), 0));
        ImGui::EndDisabled();
        if (ev.risk != ET::Risk::Safe) ImGui::PopStyleColor();
        const bool hoveredBtn = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::SameLine();
        char dayText[32];
        if (ev.dayZ >= 0) l10n::Fmt(dayText, sizeof(dayText), l10n::T("day %d"), ev.dayZ);
        else std::snprintf(dayText, sizeof(dayText), "-");
        ImGui::TextDisabled("%-7s", dayText);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.95f, 1.0f));
        ImGui::Text("%-9s", TimeText(ev.time));
        ImGui::PopStyleColor();
        bool forceClicked = false;
        const EF::BoxStatus box = authority ? EF::StatusFor(ev.name) : EF::BoxStatus{};
        if (box.hasBox) {
            ImGui::SameLine();
            if (!box.resolved) ImGui::TextDisabled("%s", l10n::T("[volume ?]"));
            else if (box.shots == 0) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", l10n::T("[FIRED]"));
            else if (box.armed) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", l10n::T("[ARMED - walk in]"));
            else ImGui::TextDisabled("%s", l10n::T("[volume-gated]"));
            ImGui::SameLine();
            ImGui::PushID(ev.name);
            forceClicked = ImGui::SmallButton(l10n::Label(l10n::T("NOW!"), "force_now"));
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", mech);
        if (hoveredBtn) {
            char tip[512];
            if (danger)
                l10n::Fmt(tip, sizeof(tip), l10n::T("%1$s  |  %2$s\nCan wreck a save: Ctrl+click to trigger."), catName, mech);
            else
                l10n::Fmt(tip, sizeof(tip), l10n::T("%1$s  |  %2$s"), catName, mech);
            ImGui::SetTooltip("%s", tip);
        }
        if (clicked && (!danger || ImGui::GetIO().KeyCtrl)) CH::FireEvent(ev.name);
        if (forceClicked && (!danger || ImGui::GetIO().KeyCtrl)) EF::ForceNow(ev.name);
    }
    ImGui::EndChild();
}

}  // namespace ui::cheat_panes
