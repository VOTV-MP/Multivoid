// ui/server_settings_pane.cpp -- see ui/server_settings_pane.h.
//
// One drawer for every kind: the row's label with its desc as the (?) tooltip, a control by kind,
// a Reset button, and the note under a row that is not live. Every change is a command line
// through command_sync::Submit; no setter is called here.
//
// The pane keeps, per row, the last value text it saw and the text of its input box. When the
// row's value moves (a command's own reply, an ini edit, a session start) the box is brought to
// it, unless the person is typing in it: their text stays until they apply or leave. The box
// counts as typed in only if the row was drawn on the previous frame, so a pane closed while a
// box was active starts unfocused on reopen. A number or string row also draws its current value
// as text beside the box, so an edit the server refused or the person abandoned is never mistaken
// for the value in force.

#include "ui/server_settings_pane.h"

#include "coop/commands/command_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "l10n/l10n.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ui::server_settings_pane {
namespace {

using ui::scale::S;
namespace CR = coop::config_registry;
namespace CFG = coop::config;

constexpr size_t kEditMax = 256;
constexpr size_t kTokenMax = 64;

// Render thread only.
struct RowState {
    std::string seen;           // the last EffectiveText this pane saw
    char edit[kEditMax] = {};   // the number or string box's text
    bool focused = false;       // the box was being typed in when last drawn
    int lastFrame = -1;         // the ImGui frame this row was last drawn on
    bool primed = false;        // `seen` and `edit` hold the row's value at least once
};
std::unordered_map<const CR::Row*, RowState> g_state;

void SubmitLine(std::string line) { coop::command_sync::Submit(std::move(line)); }

void Sync(RowState& st, const std::string& now) {
    if (st.primed && now == st.seen) return;
    const bool first = !st.primed;
    st.primed = true;
    st.seen = now;
    if (first || !st.focused) std::snprintf(st.edit, sizeof(st.edit), "%s", now.c_str());
}

bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + ('a' - 'A')) : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + ('a' - 'A')) : b[i];
        if (x != y) return false;
    }
    return true;
}

// Calls `fn(token)` for each token of an Enum row's '|'-separated list.
template <class Fn>
void ForEachToken(const char* list, Fn fn) {
    const std::string_view all(list);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t bar = all.find('|', pos);
        if (bar == std::string_view::npos) bar = all.size();
        fn(all.substr(pos, bar - pos));
        pos = bar + 1;
    }
}

// A flag draws what the reader would answer: its verdict, or the row's default for a value it
// would refuse.
void DrawFlag(const CR::Row& row, const std::string& now) {
    const int verdict = CFG::FlagVerdict(now);
    bool on = verdict > 0 ? true : verdict < 0 ? false : row.defB;
    if (ImGui::Checkbox("##value", &on)) SubmitLine(std::string("set ") + row.key + (on ? " 1" : " 0"));
}

// A number or a free string: a box and an Apply button.
void DrawTyped(const CR::Row& row, RowState& st, const std::string& now) {
    ImGui::SetNextItemWidth(S(180.0f));
    ImGui::InputText("##value", st.edit, sizeof(st.edit));
    st.focused = ImGui::IsItemActive();
    ImGui::SameLine();
    if (ImGui::SmallButton(l10n::Label(l10n::Tc("f1", "Apply"), "apply")))
        SubmitLine(std::string("set ") + row.key + " " + st.edit);
    ImGui::SameLine();
    char nowLine[kEditMax + 64];
    l10n::Fmt(nowLine, sizeof(nowLine), l10n::T("now: %s"), now.c_str());
    ImGui::TextUnformatted(nowLine);
}

// An enum: a combo of the row's tokens, the one equal to the cooked current value selected.
void DrawEnum(const CR::Row& row, const std::string& now) {
    const std::string cooked = CFG::CookValue(now);
    std::string current;
    ForEachToken(row.tokens, [&](std::string_view token) {
        if (current.empty() && EqualNoCase(token, cooked)) current = std::string(token);
    });
    ImGui::SetNextItemWidth(S(180.0f));
    if (!ImGui::BeginCombo("##value", current.c_str())) return;
    ForEachToken(row.tokens, [&](std::string_view token) {
        char text[kTokenMax];
        std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(token.size()), token.data());
        const bool selected = EqualNoCase(token, cooked);
        if (ImGui::Selectable(text, selected) && !selected)
            SubmitLine(std::string("set ") + row.key + " " + text);
        if (selected) ImGui::SetItemDefaultFocus();
    });
    ImGui::EndCombo();
}

void DrawRow(const CR::Row& row, const char* label) {
    ImGui::PushID(&row);
    RowState& st = g_state[&row];
    const int frame = ImGui::GetFrameCount();
    if (st.lastFrame != frame - 1) st.focused = false;
    st.lastFrame = frame;
    const std::string now = CFG::EffectiveText(row);
    Sync(st, now);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(l10n::T(label));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(S(420.0f));
        ImGui::TextUnformatted(l10n::T(row.desc));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    switch (row.kind) {
        case CR::Kind::Flag: DrawFlag(row, now); break;
        case CR::Kind::Int:
        case CR::Kind::Float:
        case CR::Kind::String: DrawTyped(row, st, now); break;
        case CR::Kind::Enum: DrawEnum(row, now); break;
        // A labelled row is never Identity: config_registry.cpp static_asserts it.
        case CR::Kind::Identity: break;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(l10n::Label(l10n::Tc("f1", "Reset"), "reset")))
        SubmitLine(std::string("reset ") + row.key);
    if (!CR::IsLive(&row)) ImGui::TextDisabled("%s", l10n::T("Takes effect at the next session."));
    ImGui::Spacing();
    ImGui::PopID();
}

}  // namespace

void Render() {
    size_t count = 0;
    const CR::Row* rows = CR::Rows(count);
    int drawn = 0;
    for (size_t i = 0; i < count; ++i) {
        const CR::Row* row = &rows[i];
        const char* label = CR::RowLabel(row);
        // A credential row is never drawn here: no credential is set from this pane or a command.
        if (!CR::IsServerScope(row) || label == nullptr || CR::IsCredentialKey(row->key)) continue;
        DrawRow(*row, label);
        ++drawn;
    }
    if (drawn == 0) ImGui::TextDisabled("%s", l10n::T("This server has no settings to change here."));
}

}  // namespace ui::server_settings_pane
