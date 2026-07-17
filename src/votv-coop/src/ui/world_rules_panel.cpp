// ui/world_rules_panel.cpp -- see ui/world_rules_panel.h.

#include "ui/world_rules_panel.h"

#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/core/game_thread.h"
#include "ui/scale.h"

#include "imgui.h"

#include <mutex>

namespace ui::world_rules_panel {
namespace {

using ui::scale::S;
namespace GR = ue_wrap::game_rules;
namespace GT = ue_wrap::game_thread;

// The reflected read (GR::ReadLocal) is game-thread only; the render runs on the
// render thread. We snapshot once per (re)open onto the game thread and paint the
// cached copy. The mutex only ever guards a cold, once-per-open struct copy -- it
// never contends with the per-frame render read.
std::mutex   g_mx;
GR::Snapshot g_snap;             // guarded by g_mx
bool         g_pending = false;  // a game-thread read is in flight (guarded)
int          g_lastFrame = -1000;

// Post a one-shot game-thread snapshot into g_snap (deduped while one is queued).
void RequestSnapshot() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_pending) return;
        g_pending = true;
    }
    GT::Post([] {
        GR::Snapshot s;
        GR::ReadLocal(s);  // s.valid stays false if the GameInstance isn't up yet
        std::lock_guard<std::mutex> lk(g_mx);
        g_snap = std::move(s);
        g_pending = false;
    });
}

}  // namespace

void Render() {
    // (Re)open edge: Render() wasn't called last frame -> the tab was just
    // selected -> re-snapshot so the panel always shows current values on open
    // (no per-frame walk, no poll).
    const int frame = ImGui::GetFrameCount();
    if (frame > g_lastFrame + 1) RequestSnapshot();
    g_lastFrame = frame;

    if (ImGui::SmallButton("Refresh")) RequestSnapshot();
    ImGui::SameLine();
    ImGui::TextDisabled("Read-only -- the world rules this peer is running under.");

    GR::Snapshot snap;
    bool pending = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        snap = g_snap;
        pending = g_pending;
    }

    if (!snap.valid) {
        ImGui::Spacing();
        ImGui::TextDisabled(pending ? "Reading rules..."
                                    : "World rules unavailable (still loading?).");
        return;
    }

    ImGui::Spacing();
    ImGui::Text("Gamemode:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.00f, 1.00f), "%s",
                       snap.gamemodeName.empty() ? "?" : snap.gamemodeName.c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("##world_rules", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, S(360.f)))) {
        ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, S(96.f));
        for (const GR::RuleField& f : snap.fields) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(f.label.c_str());
            ImGui::TableSetColumnIndex(1);
            switch (f.kind) {
                case GR::Kind::Bool:
                    if (f.bval) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.f), "On");
                    else        ImGui::TextDisabled("Off");
                    break;
                case GR::Kind::Float:
                    ImGui::Text("%.2f", f.fval);
                    break;
                case GR::Kind::Enum:
                    ImGui::Text("#%d", f.ival);  // VOTV strips enum display names in the cook
                    break;
            }
        }
        ImGui::EndTable();
    }
}

}  // namespace ui::world_rules_panel
