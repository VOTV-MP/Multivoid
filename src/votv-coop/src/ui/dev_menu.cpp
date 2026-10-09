// ui/dev_menu.cpp -- see ui/dev_menu.h.

#include "ui/dev_menu.h"

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
#include "coop/comms/peer_action_feed.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/config/config.h"
#include "ui/fonts.h"
#include "ui/overlay_backend.h"  // Kind() -- the "Graphics API" line
#include "coop/player/roster.h"  // LocalIsHost -- the Administration role gate
#include "ui/admin_panel.h"
#include "ui/world_rules_panel.h"  // F1 > World > Rules (shown to everyone)
#include "ui/net_stats_panel.h"
#include "ui/scale.h"
#include "ui/bug_report_pane.h"
#include "ui/server_settings_pane.h"
#include "ui/skins_panel.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

namespace ui::dev_menu {
namespace {

using ui::scale::S;

// One control in the content pane. `render` draws its own ImGui widget(s) so an
// item can be a button, a checkbox reflecting live state, a slider, etc. `dev`
// hides it unless the dev switch is on; `host` hides it unless the local peer
// is the HOST of a live coop session (a ROLE gate, not a dev gate -- the
// Administration category).
struct Item { void (*render)(); bool dev; };
struct Sub  { const char* name; std::vector<Item> items; bool dev; bool host = false; };
struct Cat  { const char* name; std::vector<Sub> subs;  bool dev; bool host = false; };

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
bool StrContainsI(const char* hay, const char* needle) {
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

// Skins: the model browser. Its own panel file -- the tiles and the preview cache live in
// ui/skins_panel.cpp; this is just the tree hook.
void RenderSkins() { ui::skins_panel::Render(); }

// Network stats overlay pref + live readout (its own panel file -- ui/net_stats_panel.cpp;
// this is just the tree hook). Non-dev: every player gets the toggle, like Cosmetics.
void RenderNetStats() { ui::net_stats_panel::RenderMenuPref(); }

// Peer action notifications: show a chat/feed line when another player does a shared
// action everyone should see (first: deleting an email). A LOCAL view preference
// (each peer decides whether IT sees these lines), persisted (multivoid.ini
// ui.chat.peer_actions); the checkbox sets the row and peer_action_feed follows it.
// Rendered locally from the existing wire event -- no extra traffic; see
// coop::peer_action_feed.
void RenderChatPref() {
    bool on = coop::peer_action_feed::Enabled();
    if (ImGui::Checkbox("Peer action notifications", &on))
        coop::config::SetValue(::coop::config_registry::rows::ui_chat_peer_actions,
                               on ? "1" : "0");
    ImGui::TextDisabled("Show a chat line when another player does a shared action");
    ImGui::TextDisabled("(e.g. deletes an email). Local preference; persists across");
    ImGui::TextDisabled("sessions (ui.chat.peer_actions).");
}

// The local player's plate-visibility preference. SYNCED (a live NameplateChange plus the
// Join prefs byte for late joiners) and persisted (multivoid.ini nameplate=).
void RenderNameplatePref() {
    bool on = coop::nameplate::LocalVisible();
    if (ImGui::Checkbox("Show my nameplate to other players", &on))
        coop::config::SetValue(::coop::config_registry::rows::nameplate, on ? "1" : "0");
    ImGui::TextDisabled("Off = your floating name/health bar disappears on every peer's");
    ImGui::TextDisabled("screen -- synced live and to late joiners; persists across sessions.");

    // The local player's nick COLOUR preference. SYNCED (a live NickColorChange plus the Join
    // colour field for late joiners) and persisted (multivoid.ini nick_color=). Committed on a
    // debounce after the last edit rather than per drag frame, since each commit persists and
    // announces over the wire; the debounce and its reason are at the picker below.
    ImGui::Spacing();
    ImGui::SeparatorText("Nickname color");
    const uint32_t cur = coop::nick_color::LocalPacked();
    bool custom = coop::nick_color::IsCustom(cur);
    // The picker's WORKING color: re-seeded only when the live pref changes
    // underneath us (boot/ini or a commit), never per frame -- a per-frame
    // reseed would snap the swatch back mid-drag (the pref commits debounced).
    static float    sCol[3] = { 1.f, 1.f, 1.f };
    static uint32_t sSeen = 0xFFFFFFFFu;  // never a valid packed value -> first frame seeds
    static bool     sDirty = false;       // an uncommitted picker edit is pending
    static double   sLastEditAt = 0.0;
    if (!sDirty && cur != sSeen) {
        sSeen = cur;
        if (coop::nick_color::IsCustom(cur)) {
            sCol[0] = coop::nick_color::R(cur) / 255.f;
            sCol[1] = coop::nick_color::G(cur) / 255.f;
            sCol[2] = coop::nick_color::B(cur) / 255.f;
        }
    }
    const auto packWorking = [&] {
        return coop::nick_color::Pack(static_cast<uint8_t>(sCol[0] * 255.f + 0.5f),
                                      static_cast<uint8_t>(sCol[1] * 255.f + 0.5f),
                                      static_cast<uint8_t>(sCol[2] * 255.f + 0.5f));
    };
    if (ImGui::Checkbox("Custom nickname color", &custom)) {
        sDirty = false;  // the toggle IS the commit
        coop::config::SetValue(::coop::config_registry::rows::nick_color,
                               coop::nick_color::IniTextFor(custom ? packWorking() : 0u).c_str());
    }
    if (coop::nick_color::IsCustom(coop::nick_color::LocalPacked())) {
        ImGui::SetNextItemWidth(S(220.f));
        // Commit DEBOUNCED on the picker's own value-changed signal, because
        // IsItemDeactivatedAfterEdit does not fire reliably on a composite picker and
        // the colour would only apply after re-toggling the checkbox. Every edit
        // re-arms a short timer; ~0.35 s after the last change the pref persists +
        // announces ONCE (no per-drag-frame wire spam).
        if (ImGui::ColorPicker3("##nickcolor", sCol,
                                ImGuiColorEditFlags_PickerHueWheel |
                                    ImGuiColorEditFlags_NoSidePreview |
                                    ImGuiColorEditFlags_NoAlpha)) {
            sDirty = true;
            sLastEditAt = ImGui::GetTime();
        }
        if (sDirty && ImGui::GetTime() - sLastEditAt > 0.35) {
            sDirty = false;
            const uint32_t packed = packWorking();
            sSeen = packed;  // the commit lands async (GT hop); don't re-seed meanwhile
            coop::config::SetValue(::coop::config_registry::rows::nick_color,
                                   coop::nick_color::IniTextFor(packed).c_str());
        }
    }
    ImGui::TextDisabled("Colors your nick everywhere it shows -- nameplate, chat, player list --");
    ImGui::TextDisabled("on every peer's screen. Synced live and to late joiners; persists.");
}

// Overlay fonts -- GRANULAR per surface: chat, the net-stats widget, the nameplates
// and the menu/panels each pick their OWN family. A combo sets the ui.font.<role> row; the
// render thread applies it and rebuilds the atlas on its next frame.
void RenderFontPref() {
    namespace F = ui::fonts;
    const char* famItems[F::kFamilyCount];
    for (int i = 0; i < F::kFamilyCount; ++i)
        famItems[i] = F::FamilyLabel(static_cast<F::Family>(i));
    ImGui::TextUnformatted("Overlay fonts (per surface):");
    for (int r = 0; r < F::kRoleCount; ++r) {
        const auto role = static_cast<F::Role>(r);
        int cur = static_cast<int>(F::RoleFamily(role));
        ImGui::PushID(r);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
        if (ImGui::Combo(F::RoleLabel(role), &cur, famItems, F::kFamilyCount))
            coop::config::SetValue(coop::config_registry::FontRoleRow(static_cast<size_t>(r)),
                                   F::FamilyToken(static_cast<F::Family>(cur)));
        ImGui::PopID();
    }
    ImGui::TextDisabled("Each surface picks its own family; applies on the next frame.");
    ImGui::TextDisabled("Saved to multivoid.ini (ui.font.menu/chat/net/nameplate/toast,");
    ImGui::TextDisabled("each with its own default). Fixedsys (VOTV) = game terminal pixel");
    ImGui::TextDisabled("font; JetBrains/Cascadia monospace; Roboto proportional.");

    // UI size: a multiplier on top of the resolution factor. Applied on slider
    // RELEASE, not per drag frame -- each apply re-bakes the font atlas (a
    // one-frame ~100 ms hitch).
    ImGui::Spacing();
    ImGui::SeparatorText("UI size");
    // sPending is the handle while it is dragged; sSeen is the applied value it last mirrored. The
    // render thread applies the row on its next frame, so the slider follows the applied value.
    static float sPending = ui::scale::UserScale();
    static float sSeen = -1.f;      // never a valid scale -> the first frame syncs
    ImGui::SetNextItemWidth(S(260.f));
    ImGui::SliderFloat("##uiscale", &sPending, ui::scale::UserScaleMin(),
                       ui::scale::UserScaleMax(), "%.2fx");
    if (!ImGui::IsItemActive() && ui::scale::UserScale() != sSeen)
        sPending = sSeen = ui::scale::UserScale();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const auto& scaleRow = *::coop::config_registry::rows::ui_scale.row;
        if (sPending < static_cast<float>(scaleRow.lo)) sPending = static_cast<float>(scaleRow.lo);
        if (sPending > static_cast<float>(scaleRow.hi)) sPending = static_cast<float>(scaleRow.hi);
        char v[16];
        std::snprintf(v, sizeof(v), "%.2f", sPending);
        coop::config::SetValue(::coop::config_registry::rows::ui_scale, v);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the WHOLE overlay (menus, chat, nameplates) on top of the\n"
                          "automatic resolution scale. Applies when you release the slider\n"
                          "(the fonts re-bake). Saved to multivoid.ini (ui.scale).");
    ImGui::TextDisabled("Everything scales with the screen automatically; this is your extra zoom.");
}

// F1 > Administration > Players (host-role-gated; ui/admin_panel owns the pane).
void RenderAdminPlayers() { ui::admin_panel::Render(); }

// F1 > World > Rules (non-dev, non-host -- shown to EVERYONE; ui/world_rules_panel
// owns the pane: the rules in force on this peer, laid out as the game's own pane).
void RenderWorldRules() { ui::world_rules_panel::Render(); }

// ---- the strict nested taxonomy (refined as features land) -------------------
// Organized by the GAME'S OWN DOMAINS: Player (the person) ; World (the simulation
// state: rules/weather/clock/economy) ; Content (the game's spawnable/triggerable
// content: entities + events) ; Network ; Administration ; Cosmetics ; Report a bug
// (the bug-report form) -- there is no catch-all category, per the folder-concept
// rule. Network subs are placeholders.
const std::vector<Cat>& Tree() {
    static const std::vector<Cat> kTree = {
        { "Player", {
            { "Movement", { { &RenderTeleportClients, true }, { &RenderFreecam, true } }, true },
            { "Vitals",   { { &RenderRestoreVitals, true } }, true },
            { "HUD",      { { &RenderPosHud, true }, { &RenderObjectOverlay, true }, { &RenderRagdollBones, true } }, true },
        }, true },
        { "World", {
            // Rules is for EVERYONE (host+clients+solo): a read-only view of the
            // world rules this peer runs under (mainGameInstance.gameRules) +
            // gamemode. Non-dev, non-host.
            { "Rules",    { { &RenderWorldRules, false } }, false },
            { "Weather",  { { &RenderSnow, true } }, true },
            { "Clock",    { { &RenderSetClock, true } }, true },
            { "Economy",  { { &RenderGivePoints, true } }, true },
        }, false },
        { "Content", {
            // The game's spawnable/triggerable content. Entities = creature/entity
            // test spawns (the owner-entity eyer included); Props = prop-content
            // tools, the Q spawn-menu unlock among them, which is its own subsection
            // because it spawns PROPS rather than entities; Events = the event board.
            { "Entities", { { &RenderSpawnNpc, true } }, true },
            { "Props",    { { &RenderSpawnMenuUnlock, true } }, true },
            { "Events",   { { &RenderEvents, true } }, true },
        }, true },
        { "Network", {
            // Stats is for EVERYONE (the overlay toggle + live session readout);
            // Session stays a dev placeholder, hidden for regular players.
            { "Stats",    { { &RenderNetStats, false } }, false },
            { "Session",  {}, true },
        }, false },
        { "Administration", {
            // HOST-role-gated (not dev): online/offline/banned player admin.
            { "Players", { { &RenderAdminPlayers, false } }, false, true },
            { "Server settings", { { &ui::server_settings_pane::Render, false } }, false, true },
        }, false, true },
        { "Cosmetics", {
            { "Skins",     { { &RenderSkins, false } }, false },
            { "Nameplate", { { &RenderNameplatePref, false } }, false },
            { "Chat",      { { &RenderChatPref, false } }, false },
            { "Interface", { { &RenderFontPref, false } }, false },
        }, false },
        { "Report a bug", {
            { "Report", { { &ui::bug_report_pane::Render, false } }, false },
        }, false },
    };
    return kTree;
}

const Cat* g_selCat = nullptr;
const Sub* g_selSub = nullptr;

// A pane asked for by name (RequestSelect), taken by the next Render. The names are written before
// the flag, and read only after it is seen set.
std::string       g_wantCat, g_wantSub;
std::atomic<bool> g_wantSelect{false};

// Dev switch, read ONCE at boot by Init() (off the render thread). devMode=false
// -> only non-dev (Cosmetics) shows; the menu itself is always available.
bool g_devMode = false;

}  // namespace

void Init() {
    // Dev items show only when the dev switch is on AND the master isn't killed --
    // [dev] enabled=0 forces every dev feature off (this replaces the per-module
    // MasterEnabled() checks the migrated dev modules used to do in their Init).
    g_devMode = ::coop::config::MasterEnabled() &&
                ::coop::config::ResolveFlag(::coop::config_registry::rows::devkeys);
}

bool DevMode() {
    // The dev switch (this machine's own) AND something to show: host authority, or a local grant
    // from the host. Render-thread safe.
    return g_devMode &&
           (::coop::dev_gate::Allowed() || ::coop::session::local_grants::AnyDevLocal());
}

void RequestSelect(const char* category, const char* sub) {
    if (!category || !sub || g_wantSelect.load(std::memory_order_acquire)) return;
    g_wantCat = category;
    g_wantSub = sub;
    g_wantSelect.store(true, std::memory_order_release);
}

void Render() {
    const bool devMode = DevMode();
    const bool isHost  = coop::roster::LocalIsHost();  // lock-free, render-thread safe
    const auto& tree = Tree();

    if (g_wantSelect.exchange(false, std::memory_order_acq_rel)) {
        for (const auto& cat : tree) {
            if (g_wantCat != cat.name || (cat.dev && !devMode) || (cat.host && !isHost)) continue;
            for (const auto& sub : cat.subs) {
                if (g_wantSub != sub.name || (sub.dev && !devMode) || (sub.host && !isHost)) continue;
                g_selCat = &cat;
                g_selSub = &sub;
            }
        }
    }

    // A host-gated selection must not linger after the role drops (session stop /
    // becoming a client): reset the pane back to the picker.
    if (!isHost && g_selCat && (g_selCat->host || (g_selSub && g_selSub->host))) {
        g_selCat = nullptr;
        g_selSub = nullptr;
    }

    // Wider default plus a MIN-size constraint, so the content panel is never cramped enough to
    // need widening by hand on every open. The minimum applies even when imgui.ini saved a
    // narrower size, so an already-too-narrow window self-corrects on the next open and can
    // still be grown.
    ImGui::SetNextWindowSize(ImVec2(S(820.f), S(460.f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(720.f), S(320.f)), ImVec2(100000.0f, 100000.0f));
    if (!ImGui::Begin("VOTV Coop  -  Menu (F1)", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Left: nav tree. A major category renders as a FRAMED, accent-coloured section
    // bar (solid slate frame + light-blue label + taller row) so it clearly outranks
    // its plain, indented sub-category items below. NoTreePushOnOpen -> we own the
    // child indent explicitly (no TreePop bookkeeping).
    ImGui::BeginChild("##nav", ImVec2(S(185.f), 0), ImGuiChildFlags_Borders);
    bool firstCat = true;
    for (const auto& cat : tree) {
        if (cat.dev && !devMode) continue;
        if (cat.host && !isHost) continue;
        if (!firstCat) ImGui::Spacing();
        firstCat = false;
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.20f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.30f, 0.38f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.30f, 0.35f, 0.45f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.55f, 0.82f, 1.00f, 1.00f));
        const bool open = ImGui::TreeNodeEx(cat.name,
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen |
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopStyleColor(4);
        if (!open) continue;
        ImGui::Indent(S(14.0f));
        for (const auto& sub : cat.subs) {
            if (sub.dev && !devMode) continue;
            if (sub.host && !isHost) continue;
            const bool selected = (g_selSub == &sub);
            if (ImGui::Selectable(sub.name, selected)) { g_selCat = &cat; g_selSub = &sub; }
        }
        ImGui::Unindent(S(14.0f));
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: the selected subcategory's controls.
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_selCat && g_selSub) {
        ImGui::TextDisabled("%s  >  %s", g_selCat->name, g_selSub->name);
        ImGui::Separator();
        int shown = 0;
        for (const auto& it : g_selSub->items) {
            if (it.dev && !devMode) continue;
            if (it.render) { it.render(); ++shown; }
        }
        if (shown == 0) ImGui::TextDisabled("No tools here yet -- coming soon.");
    } else {
        ImGui::TextDisabled("Select a category on the left.");
        // Which graphics API this session is running on (the overlay knows
        // because it renders through it). Answers "why does X look/behave
        // different for me" without asking the user to dig through logs.
        {
            const char* rhi = ui::overlay_backend::Kind();
            ImGui::Spacing();
            ImGui::TextDisabled("Graphics API: %s", rhi ? rhi : "starting up");
        }
        if (!devMode) {
            ImGui::Spacing();
            if (g_devMode && !::coop::dev_gate::Allowed())
                ImGui::TextWrapped("As a client you see only what the host allows you.");
            else
                ImGui::TextWrapped("Developer tools are hidden. Set [dev] devkeys=1 in "
                                   "multivoid.ini to show them.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace ui::dev_menu
