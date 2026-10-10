// ui/loading_screen.cpp -- see ui/loading_screen.h.

#include "ui/loading_screen.h"

#include "coop/net/protocol.h"  // HostJoinPhase -- the beacon's phase, as the View carries it
#include "coop/session/join_progress.h"
#include "l10n/l10n.h"
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
        CenteredText(l10n::T("MULTIPLAYER"));
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

        // Status line: the phase, and inside Connecting the stage, each with its own sentence, so
        // a joiner can tell which of the six steps before the first download byte it is waiting in.
        // The sentence is looked up and formatted without the animated trailing dots, which are
        // not language: they follow it, so a translation never carries or reorders them.
        char body[256];
        const char* text = body;
        bool dotted = true;
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
        char d[4] = {0};
        for (int i = 0; i < dots; ++i) d[i] = '.';
        const char* host = v.host.empty() ? l10n::T("the host") : v.host.c_str();
        if (v.mode == jp::Mode::Host) {
            // HOST boot (the Host-Game flow): loading our OWN world before we host.
            if (!v.host.empty())
                l10n::Fmt(body, sizeof(body), l10n::T("Starting your server -- loading %s"), v.host.c_str());
            else
                text = l10n::T("Starting your server");
        } else if (v.phase == jp::Phase::Connecting) {
            switch (v.stage) {
            case jp::Stage::FindingHost:
                l10n::Fmt(body, sizeof(body), l10n::T("Looking up %s"), host);
                break;
            case jp::Stage::FindingRoute:
                l10n::Fmt(body, sizeof(body), l10n::T("Finding a route to %s"), host);
                break;
            case jp::Stage::ProvingIdentity:
                text = l10n::T("Exchanging identities with the host");
                break;
            case jp::Stage::Joining:
                l10n::Fmt(body, sizeof(body), l10n::T("Joining %s"), host);
                break;
            case jp::Stage::WaitingForWorld:
                text = l10n::T("Waiting for the host to capture its world");
                break;
            default:  // Dialing, or no stage reported yet
                l10n::Fmt(body, sizeof(body), l10n::T("Connecting to %s"), host);
                break;
            }
        } else if (v.phase == jp::Phase::Downloading) {
            // Say what is happening AND that it is normal. On an internet link this
            // phase is ~17 s for a mature world and it used to read "Connecting...",
            // which is what a player interprets as "hung".
            text = l10n::T("Downloading the host's world");
        } else if (v.phase == jp::Phase::LoadingWorld) {
            // The longest stage of all (30-60 s, 120 s capped) and NOTHING reports
            // progress out of the engine's load -- so it gets the marquee, which at
            // least animates, and a label that is true.
            text = l10n::T("Loading the world");
        } else if (v.phase == jp::Phase::AwaitingWorldStream) {
            // The world is up and the wait belongs to the host now. Say which side is working, and
            // say the one thing the host reports that the player cannot otherwise see: that it is
            // holding the stream until its own world settles.
            if (v.hostPhase == static_cast<uint8_t>(coop::net::HostJoinPhase::SnapshotDeferred))
                text = l10n::T("The host is settling its world");
            else
                text = l10n::T("Waiting for the host's world");
        } else {
            text = l10n::T("Receiving world from the host");
            dotted = false;
        }
        char status[256];
        l10n::Fmt(status, sizeof(status), "%s%s", text, dotted ? d : "");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.84f, 0.90f, 1.0f));
        CenteredText(status);
        ImGui::PopStyleColor();
        // The seconds spent in this step, once they add up: a step that is merely slow reads as
        // still working rather than hung, and a stuck one is visible as such. The line is always
        // laid out, so the panel does not jump when the number appears.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
        if (v.stageMs >= 3000) {
            char since[256];
            l10n::Fmt(since, sizeof(since), l10n::T("%llu s in this step"),
                      static_cast<unsigned long long>(v.stageMs / 1000));
            CenteredText(since);
        } else {
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
        }
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

        // Progress bar.
        if (v.phase == jp::Phase::Downloading && v.totalBytes > 0) {
            const float frac = static_cast<float>(v.doneBytes) / static_cast<float>(v.totalBytes);
            char overlay[256];
            // MB, one decimal -- bytes are unreadable at this size and a percentage
            // alone does not tell the player the transfer is large by nature.
            l10n::Fmt(overlay, sizeof(overlay), l10n::T("%1$.1f / %2$.1f MB"),
                      static_cast<double>(v.doneBytes) / (1024.0 * 1024.0),
                      static_cast<double>(v.totalBytes) / (1024.0 * 1024.0));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.36f, 0.59f, 0.82f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.12f, 0.16f, 1.0f));
            ImGui::ProgressBar(frac > 1.0f ? 1.0f : frac, ImVec2(barW, 0.0f), overlay);
            ImGui::PopStyleColor(2);
        } else if (v.phase == jp::Phase::Receiving && v.total > 0) {
            const float frac = static_cast<float>(v.applied) / static_cast<float>(v.total);
            char overlay[256];
            l10n::Fmt(overlay, sizeof(overlay), l10n::T("%1$u / %2$u objects"), v.applied, v.total);
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
            CenteredText(l10n::T("A new game can take a moment to load"));
            ImGui::PopStyleColor();
        } else {
            // Cancel: abort the CLIENT join (the harness drains the request -> Stop +
            // reopen browser; a session-death flee then lands us at the main menu).
            const float btnW = S(120.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - btnW) * 0.5f);
            if (ImGui::Button(l10n::Label(l10n::Tc("hud", "Cancel"), "cancel"), ImVec2(btnW, 0.0f)))
                jp::RequestCancel();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace ui::loading_screen
