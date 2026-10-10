// ui/bug_report_pane.cpp -- see ui/bug_report_pane.h.
//
// The pane keeps the form's text in fixed char buffers (ImGui edits them in place) and the file
// list the bundle would hold. The list is read by coop::bug_report::ListEntries on the first frame
// the pane is drawn after a frame it was not drawn, so a pane left open does not stat the files
// again. The save button is disabled while a bundle is being built; a press goes through
// ValidateForm first and shows its sentence in red when the form is refused.

#include "ui/bug_report_pane.h"

#include "l10n/l10n.h"

#include "coop/bug_report/report_bundle.h"
#include "coop/bug_report/report_core.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ui/scale.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ui::bug_report_pane {
namespace {

using ui::scale::S;
namespace BR = coop::bug_report;

constexpr int kFieldLines = 6;

// Render thread only.
char g_happened[BR::kFieldMaxBytes + 1] = {};
char g_expected[BR::kFieldMaxBytes + 1] = {};
char g_contact[BR::kContactMaxBytes + 1] = {};
std::vector<BR::Entry> g_entries;   // the bundle's file list, as of the pane's last opening
int g_lastFrame = -1;               // the ImGui frame the pane was last drawn on
const char* g_formError = nullptr;  // ValidateForm's sentence for the last press, else nullptr

unsigned long long Kb(uint64_t bytes) { return (bytes + 1023) / 1024; }

void DrawField(const char* label, const char* id, char* buf, size_t size) {
    ImGui::TextUnformatted(label);
    const float h = ImGui::GetTextLineHeight() * kFieldLines + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::InputTextMultiline(id, buf, size, ImVec2(-FLT_MIN, h));
}

void DrawFiles() {
    ImGui::TextUnformatted(l10n::T("Files that will be included"));
    ImGui::Indent(S(12.0f));
    char line[256];
    for (const BR::Entry& e : g_entries) {
        if (!e.leftOut.empty()) {
            l10n::Fmt(line, sizeof(line), l10n::T("%1$s -- left out: %2$s"), e.name, e.leftOut.c_str());
            ImGui::TextDisabled("%s", line);
        } else if (e.tailOnly && e.bytesOnDisk > BR::kUe4ssTailBytes) {
            l10n::Fmt(line, sizeof(line), l10n::T("%1$s (last %2$llu KB of %3$llu KB)"), e.name,
                      Kb(BR::kUe4ssTailBytes), Kb(e.bytesOnDisk));
            ImGui::TextUnformatted(line);
        } else {
            l10n::Fmt(line, sizeof(line), l10n::T("%1$s (%2$llu KB)"), e.name, Kb(e.bytesOnDisk));
            ImGui::TextUnformatted(line);
        }
    }
    for (const char* name : BR::kMadeEntries) {
        if (std::strcmp(name, "multivoid.ini") == 0) {
            l10n::Fmt(line, sizeof(line), l10n::T("%s (without passwords)"), name);
            ImGui::TextUnformatted(line);
        } else {
            ImGui::TextUnformatted(name);
        }
    }
    ImGui::Unindent(S(12.0f));
}

void ShowInFolder(const std::wstring& zipPath) {
    const std::wstring args = L"/select,\"" + zipPath + L"\"";
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    if (r <= 32) UE_LOGW("bug_report_pane: Show in folder failed (ShellExecuteW %lld)", static_cast<long long>(r));
}

void OnSavePressed() {
    BR::Form form{g_happened, g_expected, g_contact};
    g_formError = BR::ValidateForm(form);
    if (g_formError) return;
    BR::Request(std::move(form));
}

void DrawStatus(const BR::Status& st) {
    switch (st.phase) {
        case BR::Phase::Idle:
            break;
        case BR::Phase::Building:
            ImGui::TextUnformatted(l10n::T("Saving the report..."));
            break;
        case BR::Phase::Done: {
            const std::string name = coop::text::ToUtf8(std::filesystem::path(st.zipPath).filename().wstring());
            char saved[256];
            l10n::Fmt(saved, sizeof(saved), l10n::T("Saved: %1$s (%2$llu KB)"), name.c_str(), Kb(st.zipBytes));
            ImGui::TextWrapped("%s", saved);
            if (ImGui::Button(l10n::Label(l10n::T("Show in folder"), "show_in_folder"))) ShowInFolder(st.zipPath);
            break;
        }
        case BR::Phase::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
            ImGui::TextWrapped("%s", st.error.c_str());
            ImGui::PopStyleColor();
            break;
    }
}

}  // namespace

void Render() {
    const int frame = ImGui::GetFrameCount();
    if (g_lastFrame != frame - 1) g_entries = BR::ListEntries();
    g_lastFrame = frame;

    ImGui::TextWrapped("%s", l10n::T("Saves a report file on this PC. Nothing is sent."));
    ImGui::Spacing();
    DrawField(l10n::T("What happened"), "##happened", g_happened, sizeof(g_happened));
    DrawField(l10n::T("What you expected"), "##expected", g_expected, sizeof(g_expected));
    ImGui::TextUnformatted(l10n::T("Contact (optional, e.g. a Discord name)"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##contact", g_contact, sizeof(g_contact));
    ImGui::Spacing();
    DrawFiles();
    ImGui::Spacing();

    const BR::Status st = BR::GetStatus();
    ImGui::BeginDisabled(st.phase == BR::Phase::Building);
    if (ImGui::Button(l10n::Label(l10n::T("Save report"), "save_report"))) OnSavePressed();
    ImGui::EndDisabled();
    if (g_formError) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
        ImGui::TextWrapped("%s", g_formError);
        ImGui::PopStyleColor();
    }
    DrawStatus(st);
}

}  // namespace ui::bug_report_pane
