// ui/dev_menu.cpp -- see ui/dev_menu.h.

#include "ui/dev_menu.h"

#include "coop/dev/dev_gate.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/freecam.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/pos_hud.h"
#include "coop/dev/add_points.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/set_clock.h"
#include "coop/dev/spawn_menu_unlock.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dev/teleport_client.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/session/ini_config.h"
#include "harness/config.h"
#include "ui/fonts.h"
#include "coop/player/roster.h"  // LocalIsHost -- the Administration role gate
#include "ui/admin_panel.h"
#include "ui/world_rules_panel.h"  // F1 > World > Rules (shown to everyone)
#include "ui/net_stats_panel.h"
#include "ui/scale.h"
#include "ui/skins_panel.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"

namespace ui::dev_menu {
namespace {

using ui::scale::S;

// One control in the content pane. `render` draws its own ImGui widget(s) so an
// item can be a button, a checkbox reflecting live state, a slider, etc. `dev`
// hides it unless the dev switch is on; `host` hides it unless the local peer
// is the HOST of a live coop session (a ROLE gate, not a dev gate -- the
// Administration category; user 2026-07-05).
struct Item { void (*render)(); bool dev; };
struct Sub  { const char* name; std::vector<Item> items; bool dev; bool host = false; };
struct Cat  { const char* name; std::vector<Sub> subs;  bool dev; bool host = false; };

// ---- feature controls (each calls a plain function its module exposes) -------

void RenderSnow() {
    bool on = coop::dev::force_weather::IsSnowOn();
    // Host-authoritative: the toggle is a no-op on a client (the module gates it).
    if (ImGui::Checkbox("Snow", &on)) coop::dev::force_weather::SetSnow(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(host-authoritative; clients mirror)");
}

void RenderTeleportClients() {
    if (ImGui::Button("Teleport clients to me")) coop::dev::teleport_client::TeleportClientsToHost();
    ImGui::SameLine();
    ImGui::TextDisabled("(host only)");
}

void RenderFreecam() {
    bool on = coop::dev::freecam::IsActive();
    if (ImGui::Checkbox("Freecam", &on)) coop::dev::freecam::SetActive(on);
    ImGui::TextDisabled("HOME also toggles - WASD move, Space/Ctrl up/down,");
    ImGui::TextDisabled("Shift fast, wheel speed, MMB bring player");
}

void RenderRestoreVitals() {
    if (ImGui::Button("Restore vitals (food / sleep / health)")) coop::dev::restore_vitals::Restore();
    ImGui::SameLine();
    ImGui::TextDisabled("(both peers)");
    if (ImGui::Button("Set stamina low")) coop::dev::restore_vitals::SetStaminaLow();
    ImGui::SameLine();
    ImGui::TextDisabled("(local test -- sleep/energy=10 -> exhausted; nameplate syncs)");
}

void RenderSetClock() {
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
    bool on = coop::dev::pos_hud::IsVisible();
    if (ImGui::Checkbox("Position / camera readout", &on)) coop::dev::pos_hud::SetVisible(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(on-screen overlay)");
}

void RenderObjectOverlay() {
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
    float r = OO::RadiusM();
    ImGui::SetNextItemWidth(S(170.f));
    if (ImGui::SliderFloat("Radius (m)", &r, 5.f, 100.f, "%.0f")) OO::SetRadiusM(r);
    ImGui::EndDisabled();
    ImGui::Unindent(S(22.f));
}

void RenderRagdollBones() {
    namespace RB = coop::dev::ragdoll_bone_overlay;
    bool on = RB::IsEnabled();
    if (ImGui::Checkbox("Ragdoll bone skeleton", &on)) RB::SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(lines between every bone of an ACTIVE ragdoll body)");
    ImGui::TextDisabled("The native ragdoll (C key / faint / trip) is an invisible separate actor;");
    ImGui::TextDisabled("orange = your own ragdoll, cyan = a remote peer's mirror body.");
}

void RenderSpawnNpc() {
    if (ImGui::Button("Spawn kerfurOmega (in front)")) coop::dev::spawn_npc::SpawnKerfurOmega();
    ImGui::SameLine();
    ImGui::TextDisabled("(host spawns + syncs)");
    // Increment-1 mirror test: spawn the new allowlist creatures directly (the F1
    // wisps/ventCrawler EVENTS don't reliably spawn a catchable one -- see spawn_npc).
    if (ImGui::Button("Spawn killerWisp (in front)"))  coop::dev::spawn_npc::SpawnKillerWisp();
    ImGui::SameLine();
    if (ImGui::Button("Spawn ventCrawler (in front)")) coop::dev::spawn_npc::SpawnVentCrawler();
    ImGui::SameLine();
    ImGui::TextDisabled("(mirror test: watch the client radar)");
    // v72 Killer Wisp coop cross-peer-kill test: spawn the wisp ON a client puppet so it
    // grabs the CLIENT (routes the kill there) instead of the nearest = the host.
    if (ImGui::Button("Spawn killerWisp ON client (coop kill test)"))
        coop::dev::spawn_npc::SpawnKillerWispOnClient();
}

void RenderGivePoints() {
    if (ImGui::Button("+1000 Points")) coop::dev::add_points::GivePoints(1000);
    ImGui::SameLine();
    ImGui::TextDisabled("(local balance -- afford drone orders)");
}

void RenderSpawnMenuUnlock() {
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
    // Horizontal scrollbar: the effect column can run past the panel on a narrow window, so the user can
    // scroll right rather than lose the text (the window itself is also wider + min-constrained).
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

// v93 skins: the model browser (its own panel file -- the tiles + preview cache
// live in ui/skins_panel.cpp; this is just the tree hook).
void RenderSkins() { ui::skins_panel::Render(); }

// Network stats overlay pref + live readout (its own panel file -- ui/net_stats_panel.cpp;
// this is just the tree hook). Non-dev: every player gets the toggle, like Cosmetics.
void RenderNetStats() { ui::net_stats_panel::RenderMenuPref(); }

// v94: the local player's plate-visibility pref. SYNCED (live NameplateChange +
// the Join prefs byte for late joiners) and persisted (votv-coop.ini nameplate=).
void RenderNameplatePref() {
    bool on = coop::nameplate::LocalVisible();
    if (ImGui::Checkbox("Show my nameplate to other players", &on))
        coop::nameplate::RequestLocalVisible(on);
    ImGui::TextDisabled("Off = your floating name/health bar disappears on every peer's");
    ImGui::TextDisabled("screen -- synced live and to late joiners; persists across sessions.");

    // v103 (12f): the local player's nick COLOR pref. SYNCED (live NickColorChange
    // + the Join color field for late joiners) and persisted (votv-coop.ini
    // nick_color=). Applied on picker RELEASE, not per drag frame -- each apply
    // persists + announces over the wire.
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
        coop::nick_color::RequestLocal(custom ? packWorking() : 0u);
    }
    if (coop::nick_color::IsCustom(coop::nick_color::LocalPacked())) {
        ImGui::SetNextItemWidth(S(220.f));
        // Commit DEBOUNCED on the picker's own value-changed signal (user bug
        // 2026-07-05: IsItemDeactivatedAfterEdit on the composite picker never
        // fired reliably -- the color only applied after re-toggling the
        // checkbox). Every edit re-arms a short timer; ~0.35 s after the last
        // change the pref persists + announces ONCE (no per-drag-frame wire spam).
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
            coop::nick_color::RequestLocal(packed);
        }
    }
    ImGui::TextDisabled("Colors your nick everywhere it shows -- nameplate, chat, player list --");
    ImGui::TextDisabled("on every peer's screen. Synced live and to late joiners; persists.");
}

// Overlay font family (2026-07-04, user compare ask): live switch between the
// embedded families. SetFamily persists votv-coop.ini ui.font and requests the
// atlas rebuild (applies on the next frame -- the whole overlay re-bakes).
void RenderFontPref() {
    namespace F = ui::fonts;
    const F::Family cur = F::CurrentFamily();
    ImGui::TextUnformatted("Overlay font:");
    for (int i = 0; i < F::kFamilyCount; ++i) {
        const auto f = static_cast<F::Family>(i);
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(F::FamilyLabel(f), cur == f)) F::SetFamily(f);
    }
    ImGui::TextDisabled("Applies instantly to the whole overlay (chat, menus, nameplates).");
    ImGui::TextDisabled("Saved to votv-coop.ini (ui.font). JetBrains Mono / Cascadia Code");
    ImGui::TextDisabled("are monospace; Roboto is proportional.");

    // UI size (2026-07-04, user: "все менюшки и тексты ПОБОЛЬШЕ"): a multiplier
    // on top of the resolution factor. Applied on slider RELEASE, not per drag
    // frame -- each apply re-bakes the font atlas (a one-frame ~100 ms hitch).
    ImGui::Spacing();
    ImGui::SeparatorText("UI size");
    static float sPending = -1.f;   // -1 = mirror the live value
    if (sPending < 0.f) sPending = ui::scale::UserScale();
    ImGui::SetNextItemWidth(S(260.f));
    ImGui::SliderFloat("##uiscale", &sPending, 0.75f, 1.75f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui::scale::SetUserScale(sPending);
        char v[16];
        std::snprintf(v, sizeof(v), "%.2f", ui::scale::UserScale());
        harness::config::WriteIniValue("ui.scale", v);
        sPending = ui::scale::UserScale();  // reflect the clamp
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the WHOLE overlay (menus, chat, nameplates) on top of the\n"
                          "automatic resolution scale. Applies when you release the slider\n"
                          "(the fonts re-bake). Saved to votv-coop.ini (ui.scale).");
    ImGui::TextDisabled("Everything scales with the screen automatically; this is your extra zoom.");
}

// F1 > Administration > Players (host-role-gated; ui/admin_panel owns the pane).
void RenderAdminPlayers() { ui::admin_panel::Render(); }

// F1 > World > Rules (non-dev, non-host -- shown to EVERYONE; ui/world_rules_panel
// owns the pane; reads the local mainGameInstance.gameRules read-only).
void RenderWorldRules() { ui::world_rules_panel::Render(); }

// ---- the strict nested taxonomy (refined as features land) -------------------
// Player > Movement/Vitals/HUD ; Game > Weather/Entities/Events ; Network >
// Stats/Session ; Cosmetics > Skins (the v93 model browser -- the one non-dev
// category, shown to all players). Network subs are still placeholders.
const std::vector<Cat>& Tree() {
    static const std::vector<Cat> kTree = {
        { "Player", {
            { "Movement", { { &RenderTeleportClients, true }, { &RenderFreecam, true } }, true },
            { "Vitals",   { { &RenderRestoreVitals, true } }, true },
            { "HUD",      { { &RenderPosHud, true }, { &RenderObjectOverlay, true }, { &RenderRagdollBones, true } }, true },
        }, true },
        { "Game", {
            { "Weather",  { { &RenderSnow, true } }, true },
            { "Entities", { { &RenderSpawnNpc, true }, { &RenderSpawnMenuUnlock, true } }, true },
            { "Economy",  { { &RenderGivePoints, true } }, true },
            { "Clock",    { { &RenderSetClock, true } }, true },
            { "Events",   { { &RenderEvents, true } }, true },
        }, true },
        { "World", {
            // Rules is for EVERYONE (host+clients+solo): a read-only view of the
            // world rules this peer runs under (mainGameInstance.gameRules) +
            // gamemode. Non-dev, non-host. User ask 2026-07-08.
            { "Rules",    { { &RenderWorldRules, false } }, false },
        }, false },
        { "Network", {
            // Stats is for EVERYONE (the overlay toggle + live session readout);
            // Session stays a dev placeholder, hidden for regular players.
            { "Stats",    { { &RenderNetStats, false } }, false },
            { "Session",  {}, true },
        }, false },
        { "Administration", {
            // HOST-role-gated (not dev): online/offline/banned player admin.
            { "Players", { { &RenderAdminPlayers, false } }, false, true },
        }, false, true },
        { "Cosmetics", {
            { "Skins",     { { &RenderSkins, false } }, false },
            { "Nameplate", { { &RenderNameplatePref, false } }, false },
            { "Interface", { { &RenderFontPref, false } }, false },
        }, false },
    };
    return kTree;
}

const Cat* g_selCat = nullptr;
const Sub* g_selSub = nullptr;

// Dev switch, read ONCE at boot by Init() (off the render thread). devMode=false
// -> only non-dev (Cosmetics) shows; the menu itself is always available.
bool g_devMode = false;

}  // namespace

void Init() {
    // Dev items show only when the dev switch is on AND the master isn't killed --
    // [dev] enabled=0 forces every dev feature off (this replaces the per-module
    // MasterEnabled() checks the migrated dev modules used to do in their Init).
    g_devMode = ::coop::ini_config::MasterEnabled() &&
                ::coop::ini_config::IsIniKeyTrue("devkeys");
}

bool DevMode() {
    // The ini switch AND the live role gate: dev tools vanish the moment this
    // peer is a connected CLIENT (coop::dev_gate -- "strictly no dev on a
    // client"). Re-appear on disconnect / when hosting. Render-thread safe.
    return g_devMode && ::coop::dev_gate::Allowed();
}

void Render() {
    const bool devMode = DevMode();
    const bool isHost  = coop::roster::LocalIsHost();  // lock-free, render-thread safe
    const auto& tree = Tree();

    // A host-gated selection must not linger after the role drops (session stop /
    // becoming a client): reset the pane back to the picker.
    if (!isHost && g_selCat && (g_selCat->host || (g_selSub && g_selSub->host))) {
        g_selCat = nullptr;
        g_selSub = nullptr;
    }

    // Wider default + a MIN-size constraint so the content panel never gets cramped (user: elements
    // didn't fit, had to widen every time). The min applies even when imgui.ini saved a narrow size,
    // so it self-corrects an already-too-narrow window on next open; the user can still grow it.
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
        if (!devMode) {
            ImGui::Spacing();
            if (g_devMode && !::coop::dev_gate::Allowed())
                ImGui::TextWrapped("Developer tools are disabled while connected as a "
                                   "client (host-only).");
            else
                ImGui::TextWrapped("Developer tools are hidden. Set [dev] devkeys=1 in "
                                   "votv-coop.ini to show them.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace ui::dev_menu
