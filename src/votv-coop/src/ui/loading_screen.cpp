// ui/loading_screen.cpp -- see ui/loading_screen.h.

#include "ui/loading_screen.h"

#include "coop/session/join_progress.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>

namespace ui::loading_screen {
namespace {

namespace jp = coop::join_progress;
using ui::scale::S;

// Horizontal-centered text within the current window.
void CenteredText(const char* s) {
    const float w = ImGui::CalcTextSize(s).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    ImGui::TextUnformatted(s);
}

// A Source-style sweeping marquee for the indeterminate (Connecting) phase: a lit segment
// slides left->right across a dark track. Advances the ImGui cursor past it.
void IndeterminateBar(float barW) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p1(p0.x + barW, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(26, 30, 38, 255), S(3.0f));  // track
    const float seg = barW * 0.28f;
    const float span = barW + seg;
    const float x = std::fmod(static_cast<float>(ImGui::GetTime()) * (barW * 0.9f), span) - seg;
    const float lx0 = p0.x + (x < 0.0f ? 0.0f : x);
    const float lx1 = p0.x + ((x + seg) > barW ? barW : (x + seg));
    if (lx1 > lx0) dl->AddRectFilled(ImVec2(lx0, p0.y), ImVec2(lx1, p1.y), IM_COL32(92, 150, 210, 255), S(3.0f));
    ImGui::Dummy(ImVec2(barW, h));
}

}  // namespace

bool IsOpen() { return jp::Active(); }

void Close() { jp::Reset(); }

void Render() {
    jp::MaybeTimeout();          // failsafe each frame (cheap; no-op unless stuck)
    const jp::View v = jp::Snapshot();
    if (v.phase == jp::Phase::Idle) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float barW = S(380.0f);

    // A small CENTERED panel over the clean menu background (the menu widgets are hidden by
    // multiplayer_menu while a join is active). NOT a full-screen opaque cover -- a subtle
    // semi-transparent backing for legibility, auto-sized to the content.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22.0f), S(20.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.07f, 0.72f));

    // AlwaysAutoResize sizes the panel to its content; the 380-wide progress bar sets the
    // width. Centered via the pivot above.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("###coop_loading", nullptr, flags)) {
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.98f, 1.0f));
        CenteredText("MULTIPLAYER");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

        // Status line.
        char status[160];
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
        char d[4] = {0};
        for (int i = 0; i < dots; ++i) d[i] = '.';
        if (v.mode == jp::Mode::Host) {
            // HOST boot (the Host-Game flow): loading our OWN world before we host.
            if (!v.host.empty())
                std::snprintf(status, sizeof(status), "Starting your server -- loading %s%s", v.host.c_str(), d);
            else
                std::snprintf(status, sizeof(status), "Starting your server%s", d);
        } else if (v.phase == jp::Phase::Connecting) {
            if (!v.host.empty())
                std::snprintf(status, sizeof(status), "Connecting to %s%s", v.host.c_str(), d);
            else
                std::snprintf(status, sizeof(status), "Connecting to the host%s", d);
        } else if (v.phase == jp::Phase::Downloading) {
            // Say what is happening AND that it is normal. On an internet link this
            // phase is ~17 s for a mature world and it used to read "Connecting...",
            // which is what a player interprets as "hung".
            std::snprintf(status, sizeof(status), "Downloading the host's world%s", d);
        } else if (v.phase == jp::Phase::LoadingWorld) {
            // The longest stage of all (30-60 s, 120 s capped) and NOTHING reports
            // progress out of the engine's load -- so it gets the marquee, which at
            // least animates, and a label that is true.
            std::snprintf(status, sizeof(status), "Loading the world%s", d);
        } else {
            std::snprintf(status, sizeof(status), "Receiving world from the host");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.84f, 0.90f, 1.0f));
        CenteredText(status);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

        // Progress bar.
        if (v.phase == jp::Phase::Downloading && v.totalBytes > 0) {
            const float frac = static_cast<float>(v.doneBytes) / static_cast<float>(v.totalBytes);
            char overlay[64];
            // MB, one decimal -- bytes are unreadable at this size and a percentage
            // alone does not tell the player the transfer is large by nature.
            std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB",
                          static_cast<double>(v.doneBytes) / (1024.0 * 1024.0),
                          static_cast<double>(v.totalBytes) / (1024.0 * 1024.0));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.36f, 0.59f, 0.82f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.12f, 0.16f, 1.0f));
            ImGui::ProgressBar(frac > 1.0f ? 1.0f : frac, ImVec2(barW, 0.0f), overlay);
            ImGui::PopStyleColor(2);
        } else if (v.phase == jp::Phase::Receiving && v.total > 0) {
            const float frac = static_cast<float>(v.applied) / static_cast<float>(v.total);
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%u / %u objects", v.applied, v.total);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.36f, 0.59f, 0.82f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.12f, 0.16f, 1.0f));
            ImGui::ProgressBar(frac > 1.0f ? 1.0f : frac, ImVec2(barW, 0.0f), overlay);
            ImGui::PopStyleColor(2);
        } else {
            IndeterminateBar(barW);
        }
        ImGui::Dummy(ImVec2(0.0f, S(12.0f)));

        if (v.mode == jp::Mode::Host) {
            // HOST boot: NO Cancel button. The user is loading their OWN world to host;
            // a cancel here used to (via the client abort path) Stop the host session the
            // instant it started. The host just waits -- a new game can take a moment.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.64f, 0.72f, 1.0f));
            CenteredText("A new game can take a moment to load");
            ImGui::PopStyleColor();
        } else {
            // Cancel: abort the CLIENT join (the harness drains the request -> Stop +
            // reopen browser; a session-death flee then lands us at the main menu).
            const float btnW = S(120.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - btnW) * 0.5f);
            if (ImGui::Button("Cancel", ImVec2(btnW, 0.0f))) jp::RequestCancel();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace ui::loading_screen
