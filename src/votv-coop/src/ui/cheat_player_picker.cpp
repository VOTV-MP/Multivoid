// ui/cheat_player_picker.cpp -- see ui/cheat_player_picker.h.

#include "ui/cheat_player_picker.h"

#include "coop/player/roster.h"
#include "l10n/l10n.h"
#include "ui/scale.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace ui::cheat_picker {
namespace {

coop::roster::Snapshot g_snap;
double g_snapAt = -1.0;

void Refresh() {
    const double now = ImGui::GetTime();
    if (g_snapAt >= 0.0 && now - g_snapAt < 0.5) return;
    g_snapAt = now;
    coop::roster::GetSnapshot(g_snap);
}

const coop::roster::Row* Find(int slot) {
    for (int i = 0; i < g_snap.count; ++i)
        if (g_snap.rows[i].slot == slot && g_snap.rows[i].connected) return &g_snap.rows[i];
    return nullptr;
}

void Label(const coop::roster::Row& r, char* out, size_t n) {
    const char* nick = r.nick[0] ? r.nick : l10n::T("Remote player");
    if (r.isLocal) l10n::Fmt(out, n, l10n::T("%s (you)"), nick);
    else std::snprintf(out, n, "%s", nick);
}

}  // namespace

void Choose(int* slot, bool includeSelf) {
    Refresh();
    const coop::roster::Row* cur = Find(*slot);
    if (cur && (!cur->isLocal || includeSelf)) return;
    // The first player who is not this one, else this one.
    const coop::roster::Row* pick = nullptr;
    for (int i = 0; i < g_snap.count && !pick; ++i)
        if (g_snap.rows[i].connected && !g_snap.rows[i].isLocal) pick = &g_snap.rows[i];
    for (int i = 0; i < g_snap.count && !pick; ++i)
        if (g_snap.rows[i].connected) pick = &g_snap.rows[i];
    if (pick) *slot = pick->slot;
}

bool Combo(const char* id, int* slot, bool includeSelf) {
    Choose(slot, includeSelf);
    const coop::roster::Row* cur = Find(*slot);
    char preview[96] = "";
    if (cur) Label(*cur, preview, sizeof(preview));
    bool changed = false;
    ImGui::SetNextItemWidth(ui::scale::S(200.f));
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < g_snap.count; ++i) {
            const coop::roster::Row& r = g_snap.rows[i];
            if (!r.connected || (r.isLocal && !includeSelf)) continue;
            char text[96];
            Label(r, text, sizeof(text));
            ImGui::PushID(r.slot);
            const bool selected = r.slot == *slot;
            if (ImGui::Selectable(text, selected) && !selected) { *slot = r.slot; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool NickOf(int slot, char* out, size_t outLen) {
    Refresh();
    const coop::roster::Row* r = Find(slot);
    if (!r) return false;
    std::snprintf(out, outLen, "%s", r->nick[0] ? r->nick : l10n::T("Remote player"));
    return true;
}

int Count() {
    Refresh();
    int n = 0;
    for (int i = 0; i < g_snap.count; ++i) n += g_snap.rows[i].connected ? 1 : 0;
    return n > 0 ? n : 1;
}

const char* PlayerIdOf(int slot) {
    Refresh();
    const coop::roster::Row* r = Find(slot);
    return r ? r->playerId : "";
}

}  // namespace ui::cheat_picker
