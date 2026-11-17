// ui/dev_menu.cpp -- see ui/dev_menu.h.

#include "ui/dev_menu.h"
#include "ui/dev_panes.h"

#include "l10n/l10n.h"

#include "coop/cheats/cheat_request.h"  // ReadClientRights -- the Player tools' cheat subs
#include "coop/dev/dev_gate.h"
#include "coop/permissions/grants_core.h"
#include "coop/session/local_grants.h"
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
#include "ui/cheat_panes.h"
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
// hides it unless the dev switch is on.
struct Item { void (*render)(); bool dev; };

// Who a category or a sub is offered to: a ROLE gate, not a dev gate. Any: everyone. HostRole: the host of a live
// session (Administration, the host's switch for its players). Authority: the host or a solo game -- whoever writes
// the world (Host tools). Client: a player in someone else's game (Player tools). ClientCheats: a client whose host
// lets players use the cheat tools. The host's and a solo player's view never holds a client's gate, so the two cheat
// panels are never on screen together.
enum class Gate : uint8_t { Any, HostRole, Authority, Client, ClientCheats };

struct Sub  { const char* name; std::vector<Item> items; bool dev; Gate gate = Gate::Any; };
struct Cat  { const char* name; std::vector<Sub> subs;  bool dev; Gate gate = Gate::Any; };

// What this peer is, read once per frame and asked of every gate.
struct Viewer { bool dev, hostRole, authority, client, clientCheats; };

bool Offered(Gate g, const Viewer& v) {
    switch (g) {
        case Gate::Any: return true;
        case Gate::HostRole: return v.hostRole;
        case Gate::Authority: return v.authority;
        case Gate::Client: return v.client;
        case Gate::ClientCheats: return v.clientCheats;
    }
    return false;
}

bool Offered(const Cat& c, const Viewer& v) { return (!c.dev || v.dev) && Offered(c.gate, v); }
bool Offered(const Sub& sub, const Viewer& v) { return (!sub.dev || v.dev) && Offered(sub.gate, v); }

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
    if (ImGui::Checkbox(l10n::Label(l10n::T("Peer action notifications"), "peer_action_notifications"), &on))
        coop::config::SetValue(::coop::config_registry::rows::ui_chat_peer_actions,
                               on ? "1" : "0");
    ImGui::TextDisabled("%s", l10n::T("Show a chat line when another player does a shared action\n"
                                      "(e.g. deletes an email). Local preference; persists across\n"
                                      "sessions (ui.chat.peer_actions)."));
}

// The local player's plate-visibility preference. SYNCED (a live NameplateChange plus the
// Join prefs byte for late joiners) and persisted (multivoid.ini nameplate=).
void RenderNameplatePref() {
    bool on = coop::nameplate::LocalVisible();
    if (ImGui::Checkbox(l10n::Label(l10n::T("Show my nameplate to other players"), "show_my_nameplate_to_other_players"), &on))
        coop::config::SetValue(::coop::config_registry::rows::nameplate, on ? "1" : "0");
    ImGui::TextDisabled("%s", l10n::T("Off = your floating name/health bar disappears on every peer's\n"
                                      "screen -- synced live and to late joiners; persists across sessions."));

    // The local player's nick COLOUR preference. SYNCED (a live NickColorChange plus the Join
    // colour field for late joiners) and persisted (multivoid.ini nick_color=). Committed on a
    // debounce after the last edit rather than per drag frame, since each commit persists and
    // announces over the wire; the debounce and its reason are at the picker below.
    ImGui::Spacing();
    ImGui::SeparatorText(l10n::Label(l10n::T("Nickname color"), "nickname_color"));
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
    if (ImGui::Checkbox(l10n::Label(l10n::T("Custom nickname color"), "custom_nickname_color"), &custom)) {
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
    ImGui::TextDisabled("%s", l10n::T("Colors your nick everywhere it shows -- nameplate, chat, player list --\n"
                                      "on every peer's screen. Synced live and to late joiners; persists."));
}

// Overlay fonts -- GRANULAR per surface: chat, the net-stats widget, the nameplates
// and the menu/panels each pick their OWN family. A combo sets the ui.font.<role> row; the
// render thread applies it and rebuilds the atlas on its next frame.
void RenderFontPref() {
    namespace F = ui::fonts;
    const char* famItems[F::kFamilyCount];
    for (int i = 0; i < F::kFamilyCount; ++i)
        famItems[i] = F::FamilyLabel(static_cast<F::Family>(i));
    ImGui::TextUnformatted(l10n::T("Overlay fonts (per surface):"));
    for (int r = 0; r < F::kRoleCount; ++r) {
        const auto role = static_cast<F::Role>(r);
        int cur = static_cast<int>(F::RoleFamily(role));
        ImGui::PushID(r);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
        if (ImGui::Combo(l10n::Label(F::RoleLabel(role), "font_role"), &cur, famItems, F::kFamilyCount))
            coop::config::SetValue(coop::config_registry::FontRoleRow(static_cast<size_t>(r)),
                                   F::FamilyToken(static_cast<F::Family>(cur)));
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", l10n::T("Each surface picks its own family; applies on the next frame."));
    ImGui::TextDisabled("%s", l10n::T("Saved to multivoid.ini (ui.font.menu/chat/net/nameplate/toast,\n"
                                      "each with its own default). Fixedsys (VOTV) = game terminal pixel\n"
                                      "font; JetBrains/Cascadia monospace; Roboto proportional."));

    // UI size: a multiplier on top of the resolution factor. Applied on slider
    // RELEASE, not per drag frame -- each apply re-bakes the font atlas (a
    // one-frame ~100 ms hitch).
    ImGui::Spacing();
    ImGui::SeparatorText(l10n::Label(l10n::T("UI size"), "ui_size"));
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
        ImGui::SetTooltip("%s", l10n::T("Scales the WHOLE overlay (menus, chat, nameplates) on top of the\n"
                                        "automatic resolution scale. Applies when you release the slider\n"
                                        "(the fonts re-bake). Saved to multivoid.ini (ui.scale)."));
    ImGui::TextDisabled("%s", l10n::T("Everything scales with the screen automatically; this is your extra zoom."));
}

// F1 > Administration > Players (host-role-gated; ui/admin_panel owns the pane).
void RenderAdminPlayers() { ui::admin_panel::Render(); }

// F1 > World > Rules (non-dev, non-host -- shown to EVERYONE; ui/world_rules_panel
// owns the pane: the rules in force on this peer, laid out as the game's own pane).
void RenderWorldRules() { ui::world_rules_panel::Render(); }

// ---- the strict nested taxonomy (refined as features land) -------------------
// A player entry's name is marked for translation and shown translated (the drawer's Name below); a
// developer entry's name stays English. Either way the name itself is the selection key RequestSelect
// compares and the widget's id, so neither ever depends on the language.
// Organized by who uses it and then by the GAME'S OWN DOMAINS: Host tools (the host's cheats: items, economy,
// events, entities, the world) ; Player tools (a client's own tools, and the same cheats when the host allows them) ;
// World (the rules in force) ; Network ; Administration ; Cosmetics ; Report a bug (the bug-report form) ; and the
// developer's diagnostics under Player -- there is no catch-all category, per the folder-concept rule. Network subs
// are placeholders.
const std::vector<Cat>& Tree() {
    static const std::vector<Cat> kTree = {
        // The developer's diagnostics: hidden unless [dev] devkeys is on. What a host or a player uses is below.
        { "Player", {
            { "Vitals", { { &ui::dev_panes::RenderStaminaLow, true } }, true },
            { "HUD",    { { &ui::dev_panes::RenderObjectOverlay, true }, { &ui::dev_panes::RenderRagdollBones, true } }, true },
        }, true },
        // The host's tools (and a solo player's): the panes that write the shared world. Not a dev category: the host
        // sees it with no switch. A client never sees this category.
        { L10N_MARK("Host tools"), {
            { L10N_MARK("Items"),     { { &ui::cheat_panes::RenderItems, false } }, false },
            { L10N_MARK("Economy"),   { { &ui::cheat_panes::RenderEconomy, false } }, false },
            { L10N_MARK("Events"),    { { &ui::cheat_panes::RenderEvents, false } }, false },
            { L10N_MARK("Entities"),  { { &ui::cheat_panes::RenderEntities, false } }, false },
            { L10N_MARK("World"),     { { &ui::cheat_panes::RenderWorld, false } }, false },
            { L10N_MARK("Myself"),    { { &ui::cheat_panes::RenderMyself, false } }, false },
            { L10N_MARK("Player cheats"), { { &ui::cheat_panes::RenderPlayerCheats, false } }, false, Gate::HostRole },
        }, false, Gate::Authority },
        // A client's tools: its own body and the local tools the host grants always; the same Items, Economy and
        // Events panes as the host's when the host lets players use cheats (each use is a request the host runs).
        { L10N_MARK("Player tools"), {
            { L10N_MARK("Myself"),    { { &ui::cheat_panes::RenderMyself, false } }, false },
            { L10N_MARK("Items"),     { { &ui::cheat_panes::RenderItems, false } }, false, Gate::ClientCheats },
            { L10N_MARK("Economy"),   { { &ui::cheat_panes::RenderEconomy, false } }, false, Gate::ClientCheats },
            { L10N_MARK("Events"),    { { &ui::cheat_panes::RenderEvents, false } }, false, Gate::ClientCheats },
        }, false, Gate::Client },
        { L10N_MARK("World"), {
            // Rules is for EVERYONE (host+clients+solo): a read-only view of the world rules this peer runs under
            // (mainGameInstance.gameRules) + gamemode.
            { L10N_MARK("Rules"),    { { &RenderWorldRules, false } }, false },
        }, false },
        { L10N_MARK("Network"), {
            // Stats is for EVERYONE (the overlay toggle + live session readout);
            // Session stays a dev placeholder, hidden for regular players.
            { L10N_MARK("Stats"),    { { &RenderNetStats, false } }, false },
            { "Session",  {}, true },
        }, false },
        { L10N_MARK("Administration"), {
            // HOST-role-gated (not dev): online/offline/banned player admin.
            { L10N_MARK("Players"), { { &RenderAdminPlayers, false } }, false, Gate::HostRole },
            { L10N_MARK("Server settings"), { { &ui::server_settings_pane::Render, false } }, false, Gate::HostRole },
        }, false, Gate::HostRole },
        { L10N_MARK("Cosmetics"), {
            { L10N_MARK("Skins"),     { { &RenderSkins, false } }, false },
            { L10N_MARK("Nameplate"), { { &RenderNameplatePref, false } }, false },
            { L10N_MARK("Chat"),      { { &RenderChatPref, false } }, false },
            { L10N_MARK("Interface"), { { &RenderFontPref, false } }, false },
        }, false },
        { L10N_MARK("Report a bug"), {
            { L10N_MARK("Report"), { { &ui::bug_report_pane::Render, false } }, false },
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
// The pane the render thread last drew, as ONE value -- (category index + 1) << 16 | (sub index + 1),
// 0 before any -- stored after the pane's draw, so a reader on another thread that sees it knows that
// frame's text was looked up.
std::atomic<uint32_t> g_drawn{0};

// What the tree shows for an entry: a player entry translated, a developer entry as it is.
const char* Name(const char* name, bool dev) { return dev ? name : l10n::T(name); }

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

bool DrawnSelection(const char* category, const char* sub) {
    const uint32_t v = g_drawn.load(std::memory_order_acquire);
    if (v == 0 || !category || !sub) return false;
    const auto& tree = Tree();
    const uint32_t c = (v >> 16) - 1, s = (v & 0xFFFF) - 1;
    return c < tree.size() && s < tree[c].subs.size() && std::strcmp(tree[c].name, category) == 0 &&
           std::strcmp(tree[c].subs[s].name, sub) == 0;
}

void RequestSelect(const char* category, const char* sub) {
    if (!category || !sub || g_wantSelect.load(std::memory_order_acquire)) return;
    g_wantCat = category;
    g_wantSub = sub;
    g_drawn.store(0, std::memory_order_release);   // DrawnSelection waits for a draw after this
    g_wantSelect.store(true, std::memory_order_release);
}

void Render() {
    const bool devMode = DevMode();
    const bool authority = ::coop::dev_gate::Allowed();   // the host or a solo game (lock-free, render-thread safe)
    // The cheat settings, read once for the frame and handed to the panes: a client reads the host's switch and its own
    // right; the host's pane reads the switch only in a hosted session (solo has no such pane, and its read is a file's).
    ui::cheat_panes::CheatFrame cheats;
    const bool hostRole = coop::roster::LocalIsHost();
    if (!authority) {
        const auto rights = ::coop::cheats::ReadClientRights();
        cheats.hostAllows = rights.hostAllows;
        cheats.mayCheat = rights.Open();
    } else if (hostRole) {
        cheats.hostAllows = ::coop::cheats::PlayersMayCheat();
    }
    ui::cheat_panes::SetCheatFrame(cheats);
    const Viewer viewer{devMode, hostRole, authority, !authority, cheats.mayCheat};
    const auto& tree = Tree();

    if (g_wantSelect.exchange(false, std::memory_order_acq_rel)) {
        for (const auto& cat : tree) {
            if (g_wantCat != cat.name || !Offered(cat, viewer)) continue;
            for (const auto& sub : cat.subs) {
                if (g_wantSub != sub.name || !Offered(sub, viewer)) continue;
                g_selCat = &cat;
                g_selSub = &sub;
            }
        }
    }

    // A gated selection must not linger after the role drops (session stop, becoming a client, the host turning the
    // cheats off): reset the pane back to the picker.
    if (g_selCat && (!Offered(*g_selCat, viewer) || (g_selSub && !Offered(*g_selSub, viewer)))) {
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
        if (!Offered(cat, viewer)) continue;
        if (!firstCat) ImGui::Spacing();
        firstCat = false;
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.20f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.30f, 0.38f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.30f, 0.35f, 0.45f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.55f, 0.82f, 1.00f, 1.00f));
        ImGui::PushID(&cat);   // two entries may share a name (a category and a sub), never an id
        const bool open = ImGui::TreeNodeEx(l10n::Label(Name(cat.name, cat.dev), cat.name),
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen |
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopID();
        ImGui::PopStyleColor(4);
        if (!open) continue;
        ImGui::Indent(S(14.0f));
        for (const auto& sub : cat.subs) {
            if (!Offered(sub, viewer)) continue;
            const bool selected = (g_selSub == &sub);
            ImGui::PushID(&sub);
            if (ImGui::Selectable(l10n::Label(Name(sub.name, sub.dev), sub.name), selected)) {
                g_selCat = &cat;
                g_selSub = &sub;
            }
            ImGui::PopID();
        }
        ImGui::Unindent(S(14.0f));
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: the selected subcategory's controls.
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_selCat && g_selSub) {
        ImGui::TextDisabled("%s  >  %s", Name(g_selCat->name, g_selCat->dev), Name(g_selSub->name, g_selSub->dev));
        ImGui::Separator();
        int shown = 0;
        for (const auto& it : g_selSub->items) {
            if (it.dev && !devMode) continue;
            if (it.render) { it.render(); ++shown; }
        }
        if (shown == 0) ImGui::TextDisabled("%s", l10n::T("No tools here yet -- coming soon."));
        const uint32_t cat = static_cast<uint32_t>(g_selCat - tree.data());
        const uint32_t sub = static_cast<uint32_t>(g_selSub - g_selCat->subs.data());
        g_drawn.store(((cat + 1) << 16) | (sub + 1), std::memory_order_release);
    } else {
        ImGui::TextDisabled("%s", l10n::T("Select a category on the left."));
        // Which graphics API this session is running on (the overlay knows
        // because it renders through it). Answers "why does X look/behave
        // different for me" without asking the user to dig through logs.
        {
            // The menu draws only once the overlay is up, after a seam selected the RHI.
            const char* rhi = ui::overlay_backend::Kind();
            char line[256];
            l10n::Fmt(line, sizeof(line), l10n::T("Graphics API: %s"), rhi ? rhi : "?");
            ImGui::Spacing();
            ImGui::TextDisabled("%s", line);
        }
        if (!devMode) {
            ImGui::Spacing();
            if (g_devMode && !::coop::dev_gate::Allowed())
                ImGui::TextWrapped("%s", l10n::T("As a client you see only what the host allows you."));
            else
                ImGui::TextWrapped("%s", l10n::T("Developer tools are hidden. Set [dev] devkeys=1 in "
                                                  "multivoid.ini to show them."));
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace ui::dev_menu
