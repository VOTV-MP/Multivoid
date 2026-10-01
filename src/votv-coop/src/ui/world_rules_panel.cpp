// ui/world_rules_panel.cpp -- see ui/world_rules_panel.h.

#include "ui/world_rules_panel.h"

#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/game_rules_pane.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/engine/world_identity.h"
#include "ui/scale.h"

#include "imgui.h"
#include "coop/text/i18n.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ui::world_rules_panel {
namespace {

using ui::scale::S;
namespace GR = ue_wrap::game_rules;
namespace GP = ue_wrap::game_rules_pane;
namespace GT = ue_wrap::game_thread;

// What the render paints: the rules already joined to the game's layout, every value already text.
// Built on the game thread once per snapshot, so a frame only draws.
struct ViewRow {
    std::string key;             // the rule's struct member name: unique, so it keys the selection
    std::string label;
    std::string value;
    std::string description;     // what the game's own description pane would say for this row
    bool        on = false;      // a checkbox rule that is set: drawn in the "on" colour
    bool        isCheck = false;
    bool        greyed = false;  // the row is not editable in the game's menu: locked for this
                                 // player, or switched off by another rule
    bool        locked = false;  // ...and the reason is a lock. The game shows exactly ONE of two
                                 // overlay images: img_locked (red) when the rule is locked, and
                                 // img_disabled (white) when it is merely switched off.
};
struct ViewCategory {
    std::string          name;
    std::vector<ViewRow> rows;
};
struct View {
    bool                      valid = false;
    std::string               gamemode;
    int                       differFromSaved = 0;  // rules in force that the saved copy disagrees with
    bool                      settled = false;      // the save object and the lock inputs were both up
    std::vector<ViewCategory> categories;
};

// The reflected reads are game-thread only; the render runs on the render thread. The game thread
// publishes a finished View under the mutex and bumps the generation; the render keeps its own copy
// and takes the mutex only when the generation moved.
std::mutex       g_mx;
View             g_published;        // guarded by g_mx
GP::Pane         g_pane;             // game thread only: the game's layout, fixed for a process
uint32_t         g_paneTriedGen = 0; // game thread only: the world generation of the last failed read
bool             g_pending = false;  // a game-thread read is in flight (guarded by g_mx)
std::atomic<int> g_generation{0};
int              g_lastFrame = -1000;

GR::Kind KindOf(GP::Control c) {
    return c == GP::Control::Check ? GR::Kind::Bool : c == GP::Control::Slider ? GR::Kind::Float : GR::Kind::Enum;
}

ViewRow MakeRow(const GR::RuleField& f, const GP::Row* row, const GP::LockInputs& lock) {
    ViewRow out;
    out.key = f.key;
    out.label = (row && !row->label.empty()) ? row->label : f.label;
    char buf[32];
    switch (f.kind) {
        case GR::Kind::Bool:
            out.isCheck = true;
            out.on = f.bval;
            out.value = f.bval ? "On" : "Off";
            break;
        case GR::Kind::Float:
            std::snprintf(buf, sizeof(buf), "%.*f", row ? row->sliderDecimals : 2, f.fval);
            out.value = buf;
            break;
        case GR::Kind::Enum:
            if (f.valueName.empty()) { std::snprintf(buf, sizeof(buf), "#%d", f.ival); out.value = buf; }
            else out.value = f.valueName;
            break;
    }
    if (!row) return out;
    if (row->description.size() > 1) out.description = row->description;  // the game writes "-" for none
    // Locked as the game's menu locks it for THIS player: a lock is about who may edit the rule in
    // the menu, and the value shown is in force either way. On a locked row the game's own
    // uicomp_gameRuleSlot::getTexts REPLACES the description with the lock message, so this does
    // too, in the game's wording; the achievement list carries its [O]/[X] held marker.
    if (!GP::IsUnlocked(*row, lock)) {
        out.locked = true;
        out.greyed = true;
        if (row->unlockAchievements.empty()) {
            std::snprintf(buf, sizeof(buf), "%d", row->unlockDay);
            out.description = std::string("This rule is day-locked!\nUnlock this rule by playing past day ") + buf;
        } else {
            out.description = "This rule is achievement-locked!\nUnlock this rule by getting following achievements:";
            for (const std::string& a : row->unlockAchievements) {
                const bool held = std::find(lock.achievements.begin(), lock.achievements.end(), a) !=
                                  lock.achievements.end();
                out.description += "\n- " + a + (held ? " [O]" : " [X]");
            }
        }
    }
    return out;
}

// Game thread: read the rules, join them to the game's layout, publish.
void BuildAndPublish() {
    View view;
    GR::Snapshot snap;
    if (GR::ReadLocal(snap) && snap.valid) {
        // The layout is read until it has been read once. A failed read costs a walk of every object
        // (the widget class was not found), and whether the class is loaded can only change with a
        // world, so a failure is not tried again until the world has changed.
        const uint32_t worldGen = ue_wrap::world_identity::Generation();
        if (!g_pane.valid && g_paneTriedGen != worldGen && !GP::Read(g_pane)) g_paneTriedGen = worldGen;
        GP::LockInputs lock;
        GP::ReadLockInputs(lock);
        // Settled = the two inputs a world brings up late are both in. The layout is not one of them:
        // without it the panel lists the rules flat, and that is a final answer for this world.
        view.settled = snap.savedValid && lock.valid;
        // The game's pane greys "Permanent season" while "Enable permanent season" is off
        // (ui_gameRulesList.updChanges); the two are known here by their rule names.
        bool permanentSeasonOn = true;
        for (const GR::RuleField& f : snap.fields) if (f.key == "enablePermanentSeason") permanentSeasonOn = f.bval;
        view.valid = true;
        view.gamemode = snap.gamemodeName;
        if (snap.savedValid) {
            for (size_t i = 0; i < snap.fields.size() && i < snap.saved.size(); ++i) {
                const GR::RuleField& a = snap.fields[i];
                const GR::RuleField& b = snap.saved[i];
                if (a.bval != b.bval || a.ival != b.ival || a.fval != b.fval) ++view.differFromSaved;
            }
        }
        std::vector<bool> placed(snap.fields.size(), false);
        for (const GP::Category& cat : g_pane.categories) {
            ViewCategory vc;
            vc.name = cat.name.empty() ? "Rules" : cat.name;
            for (const GP::Row& row : cat.rows) {
                const GR::RuleField* f = GR::NthOfKind(snap.fields, KindOf(row.control), row.index);
                if (!f) continue;
                // A rule of a category the game never shows is placed, so it does not fall under "Other".
                placed[static_cast<size_t>(f - snap.fields.data())] = true;
                if (cat.hidden) continue;
                vc.rows.push_back(MakeRow(*f, &row, lock));
                if (f->key == "permanentSeason" && !permanentSeasonOn) vc.rows.back().greyed = true;
            }
            // A category the game collapses is not shown at all; one it shows is shown even with no
            // rows in it, which is how its own pane renders "Challenges".
            if (!cat.hidden) view.categories.push_back(std::move(vc));
        }
        // Rules the game's pane does not place: all of them when its layout could not be read, and
        // any rule a game update adds before its pane does.
        ViewCategory other;
        other.name = g_pane.valid ? "Other" : "Rules";
        for (size_t i = 0; i < snap.fields.size(); ++i) {
            if (!placed[i]) other.rows.push_back(MakeRow(snap.fields[i], nullptr, GP::LockInputs{}));
        }
        if (!other.rows.empty()) view.categories.push_back(std::move(other));
    }
    std::lock_guard<std::mutex> lk(g_mx);
    g_published = std::move(view);
    g_pending = false;
    g_generation.fetch_add(1, std::memory_order_release);
}

void RequestSnapshot() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_pending) return;
        g_pending = true;
    }
    GT::Post([] { BuildAndPublish(); });
}

}  // namespace

void Render() {
    // Re-snapshot on the (re)open edge (Render() wasn't called last frame -> the tab was just
    // selected) and whenever the world changed under an open panel: the rules belong to a world, and
    // a panel left open across a load would go on showing the previous one's. No per-frame walk.
    static uint32_t s_worldGen = 0;  // render thread only
    const uint32_t worldGen = ue_wrap::world_identity::Generation();
    const int frame = ImGui::GetFrameCount();
    const bool edge = frame > g_lastFrame + 1 || worldGen != s_worldGen;
    if (edge) RequestSnapshot();
    g_lastFrame = frame;
    s_worldGen = worldGen;

    if (ImGui::SmallButton(coop::i18n::Tr("Refresh"))) RequestSnapshot();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", coop::i18n::Tr("Read-only. The host's save sets the rules for everyone."));

    static View s_view;       // render thread only
    static int  s_viewGen = 0;
    const int gen = g_generation.load(std::memory_order_acquire);
    if (gen != s_viewGen) {
        std::lock_guard<std::mutex> lk(g_mx);
        s_view = g_published;
        s_viewGen = gen;
    }
    // A snapshot taken while a gameplay world was still coming up (no save object, no gamemode
    // profile yet) is retried once a second until one settles, at most kSettleRetries times per edge
    // above: a world that has not brought them up by then is not going to, and the panel shows what
    // it has. Outside a gameplay world there is nothing to wait for, so nothing is retried.
    constexpr int kSettleRetries = 20;
    static double s_nextRetry = 0.0;  // render thread only
    static int    s_retriesLeft = 0;
    if (edge) s_retriesLeft = kSettleRetries;
    const bool inWorld = ue_wrap::world_identity::CurrentWorldKind() == ue_wrap::world_identity::WorldKind::Gameplay;
    if (inWorld && !s_view.settled && s_retriesLeft > 0 && ImGui::GetTime() >= s_nextRetry) {
        s_nextRetry = ImGui::GetTime() + 1.0;
        --s_retriesLeft;
        RequestSnapshot();
    }
    if (!s_view.valid) {
        ImGui::Spacing();
        ImGui::TextDisabled(gen == 0 ? "Reading rules..." : "World rules unavailable (still loading?).");
        return;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(coop::i18n::Tr("Gamemode:"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.00f, 1.00f), "%s", s_view.gamemode.empty() ? "?" : s_view.gamemode.c_str());
    // The two copies the game keeps must agree; when they do not, this peer is not under the rules
    // its world was saved with, and saying so beats showing either copy as the truth.
    if (s_view.differFromSaved > 0) {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%d rule(s) in force differ from the rules saved with this world.",
                           s_view.differFromSaved);
    }
    ImGui::Separator();

    // The rule clicked last, by its struct member key: the game's pane describes one rule at a time.
    // Keyed by the rule, not by a position, so a re-snapshot cannot move the selection onto another
    // rule, and not by the label, which two rules could in principle share; the description is
    // re-read from the live view every frame.
    static std::string s_selected;  // render thread only
    const ViewRow*     selected = nullptr;
    for (const ViewCategory& cat : s_view.categories) {
        for (const ViewRow& row : cat.rows) {
            if (row.key == s_selected) { selected = &row; break; }
        }
        if (selected) break;
    }

    // The list scrolls; the description pane below it stays put, as the game's does.
    const float paneHeight = S(96.f);
    ImGui::BeginChild("##world_rules_scroll", ImVec2(0, -paneHeight));
    for (const ViewCategory& cat : s_view.categories) {
        if (!ImGui::CollapsingHeader(cat.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        if (cat.rows.empty()) continue;  // a category the game shows with nothing in it: header only
        if (!ImGui::BeginTable(cat.name.c_str(), 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) continue;
        ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, S(96.f));
        for (const ViewRow& row : cat.rows) {
            ImGui::TableNextRow();
            // The game lays one overlay image over the row, and only one: img_locked's red when the
            // rule is locked, img_disabled's white when it is merely switched off, each at a tenth
            // alpha. RowBg1 is the target that draws OVER the row background; RowBg0 would replace
            // the striping instead of washing it.
            if (row.greyed) {
                const ImVec4 wash = row.locked ? ImVec4(1.f, 0.f, 0.f, 0.10f) : ImVec4(1.f, 1.f, 1.f, 0.10f);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(wash));
            }
            ImGui::TableSetColumnIndex(0);
            if (row.greyed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            // SpanAllColumns, so the whole row is the click target the game's own row is.
            if (ImGui::Selectable(row.label.c_str(), s_selected == row.key, ImGuiSelectableFlags_SpanAllColumns))
                s_selected = row.key;
            if (row.greyed) ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            if (row.isCheck && row.on && !row.greyed) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.f), "%s", row.value.c_str());
            else if (row.isCheck || row.greyed)        ImGui::TextDisabled("%s", row.value.c_str());
            else                        ImGui::TextUnformatted(row.value.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginChild("##world_rules_desc", ImVec2(0, 0));
    if (!selected) {
        ImGui::TextDisabled("%s", coop::i18n::Tr("Click on the name of the rule to see its description."));
    } else {
        ImGui::TextUnformatted(selected->label.c_str());
        ImGui::PushTextWrapPos(0.f);
        if (selected->description.empty()) ImGui::TextDisabled("%s", coop::i18n::Tr("(no description)"));
        else                               ImGui::TextUnformatted(selected->description.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

}  // namespace ui::world_rules_panel
