// ui/cheat_world_pane.cpp -- F1 > Host tools > World: the weather, the clock, and helpers for the players.
//
// The host's own controls over the shared world, each a call into the module that owns it: the snow switch
// (coop/dev/force_weather), the world clock (coop/dev/set_clock), vitals (coop/dev/restore_vitals) and the moves of
// players (coop/session/teleport_client, the /tphere command, coop/cheats/self_teleport). The host writes the world
// and every client follows through the existing lanes; nothing here is sent by the pane.

#include "ui/cheat_panes.h"

#include "coop/cheats/self_teleport.h"
#include "coop/commands/command_sync.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/set_clock.h"
#include "coop/session/teleport_client.h"
#include "l10n/l10n.h"
#include "ui/cheat_player_picker.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace ui::cheat_panes {
namespace {

using ui::scale::S;

void Weather() {
    ImGui::SeparatorText(l10n::Label(l10n::T("Weather"), "weather"));
    bool on = coop::dev::force_weather::IsSnowOn();
    if (ImGui::Checkbox(l10n::Label(l10n::T("Snow"), "snow"), &on)) coop::dev::force_weather::SetSnow(on);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", l10n::T("(the clients follow)"));
    ImGui::TextDisabled("%s", l10n::T("Rain, fog and the rest of the weather are under Events > Weather."));
}

void Clock() {
    namespace SC = coop::dev::set_clock;
    ImGui::SeparatorText(l10n::Label(l10n::T("Clock"), "clock"));
    int hour = 0, minute = 0, day = 0;
    float frac = 0.f;
    if (!SC::ReadCurrent(hour, minute, day, frac)) {
        ImGui::TextDisabled("%s", l10n::T("The world clock is not available yet -- enter a world."));
        return;
    }
    char line[96];
    l10n::Fmt(line, sizeof(line), l10n::T("Now: day %1$d, %2$02d:%3$02d"), day, hour, minute);
    ImGui::TextUnformatted(line);
    static int sDay = -1, sHour = -1, sMin = -1;
    if (sDay < 0) { sDay = day; sHour = hour; sMin = minute; }   // seeded once from the live clock
    ImGui::SetNextItemWidth(S(90.f));
    ImGui::InputInt(l10n::Label(l10n::T("day"), "clock_day"), &sDay);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(74.f));
    ImGui::InputInt(l10n::Label(l10n::T("h"), "clock_hour"), &sHour);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(74.f));
    ImGui::InputInt(l10n::Label(l10n::T("m"), "clock_min"), &sMin);
    if (sDay < 1) sDay = 1;
    if (sHour < 0) sHour = 0;
    if (sHour > 23) sHour = 23;
    if (sMin < 0) sMin = 0;
    if (sMin > 59) sMin = 59;
    ImGui::SameLine();
    if (ImGui::Button(l10n::Label(l10n::T("Set clock"), "set_clock"))) SC::SetClock(sDay, sHour, sMin);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", l10n::T("Sets the game's own clock. Jumping the day forward runs every event scheduled in "
                                        "between at once."));
    float f = frac;
    ImGui::SetNextItemWidth(S(260.f));
    if (ImGui::SliderFloat(l10n::Label(l10n::T("Sun position"), "sun_position"), &f, 0.0f, 0.999f, "%.3f"))
        SC::SetTimeFraction(f);   // live: dragging moves the sun
    ImGui::TextDisabled("%s", l10n::T("Only the sun's angle; the clock above is what events and the save follow."));
}

void Players() {
    namespace RV = coop::dev::restore_vitals;
    ImGui::SeparatorText(l10n::Label(l10n::T("Players"), "players"));
    static int target = -1;
    if (ui::cheat_picker::Count() <= 1) {
        if (ImGui::Button(l10n::Label(l10n::T("Restore my vitals"), "restore_mine"))) RV::RestoreFor(0);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", l10n::T("(food, sleep and health)"));
        return;
    }
    if (ImGui::Button(l10n::Label(l10n::T("Restore everyone's vitals"), "restore_all"))) RV::Restore();
    ImGui::SameLine();
    if (ImGui::Button(l10n::Label(l10n::T("Teleport clients to me"), "tp_clients"))) coop::teleport_client::TeleportClientsToHost();

    ui::cheat_picker::Choose(&target, true);
    char nick[96] = "";
    ui::cheat_picker::NickOf(target, nick, sizeof(nick));
    ui::cheat_picker::Combo("##worldtarget", &target, true);
    ImGui::SameLine();
    char text[160];
    l10n::Fmt(text, sizeof(text), l10n::T("Restore vitals of %s"), nick);
    if (ImGui::Button(l10n::Label(text, "restore_one"))) RV::RestoreFor(target);

    const bool isHostSlot = target == 0;
    const char* id = ui::cheat_picker::PlayerIdOf(target);
    ImGui::BeginDisabled(isHostSlot || id[0] == '\0');
    l10n::Fmt(text, sizeof(text), l10n::T("Teleport %s to me"), nick);
    if (ImGui::Button(l10n::Label(text, "tp_one_to_me"))) coop::command_sync::Submit(std::string("tphere ") + id);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(isHostSlot);
    l10n::Fmt(text, sizeof(text), l10n::T("Teleport me to %s"), nick);
    if (ImGui::Button(l10n::Label(text, "tp_me_to_one"))) coop::cheats::self_teleport::ToPlayer(target);
    ImGui::EndDisabled();
}

}  // namespace

void RenderWorld() {
    if (!coop::dev_gate::Allowed()) {
        ImGui::TextDisabled("%s", l10n::T("Only the host can do this."));
        return;
    }
    Weather();
    Clock();
    Players();
}

}  // namespace ui::cheat_panes
