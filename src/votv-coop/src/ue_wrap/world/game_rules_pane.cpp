// ue_wrap/world/game_rules_pane.cpp -- see ue_wrap/world/game_rules_pane.h.

#include "ue_wrap/world/game_rules_pane.h"

#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/daynightcycle.h"

#include <algorithm>
#include <cstdint>

namespace ue_wrap::game_rules_pane {
namespace {

namespace FT = ue_wrap::ftext_utils;
namespace R = ue_wrap::reflection;

constexpr int kMaxDepth = 24;  // the pane nests 6 deep; a bound, so a cyclic tree cannot recurse forever

struct ObjArray { void** data; int32_t num; int32_t max; };
struct NameArray { R::FName* data; int32_t num; int32_t max; };

std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

// The pointer-typed property `name` of `obj`, or null.
void* ObjectField(void* obj, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off < 0 ? nullptr : *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
}

int32_t IntField(void* obj, const wchar_t* name, int32_t fallback) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off < 0 ? fallback : *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
}

std::string TextField(void* obj, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off < 0 ? std::string() : Narrow(FT::FTextToString(reinterpret_cast<uint8_t*>(obj) + off));
}

// UWidget::Visibility is ESlateVisibility: 1 Collapsed, 2 Hidden.
bool IsHidden(void* widget) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(widget), L"Visibility");
    if (off < 0) return false;
    const uint8_t v = *(reinterpret_cast<uint8_t*>(widget) + off);
    return v == 1 || v == 2;
}

Row ReadRow(void* widget) {
    Row row;
    row.label = TextField(widget, L"displayName");
    row.description = TextField(widget, L"description");
    switch (IntField(widget, L"variableType", 0)) {
        case 1:  row.control = Control::Slider; break;
        case 2:  row.control = Control::Combo; break;
        default: row.control = Control::Check; break;
    }
    row.index = IntField(widget, L"variableIndex", 0);
    row.sliderDecimals = IntField(widget, L"slider_decimals", 2);
    row.unlockDay = IntField(widget, L"unlockableAfterDay", 0);
    const int32_t achOff = R::FindPropertyOffset(R::ClassOf(widget), L"unlockableAchievements");
    if (achOff >= 0) {
        const auto* arr = reinterpret_cast<const NameArray*>(reinterpret_cast<uint8_t*>(widget) + achOff);
        for (int32_t i = 0; arr->data && i < arr->num && i < 16; ++i)
            row.unlockAchievements.push_back(Narrow(R::ToString(arr->data[i])));
    }
    return row;
}

// The first text block under `widget`: a category's header is a text block, possibly wrapped.
std::string FirstText(void* widget, int depth);

template <typename Fn>
void ForEachChild(void* widget, Fn fn) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(widget), L"Slots");  // UPanelWidget
    if (off < 0) return;
    const auto* slots = reinterpret_cast<const ObjArray*>(reinterpret_cast<uint8_t*>(widget) + off);
    for (int32_t i = 0; slots->data && i < slots->num && i < 256; ++i) {
        void* slot = slots->data[i];
        if (!slot || !R::IsLive(slot)) continue;
        if (void* content = ObjectField(slot, L"Content")) fn(content);
    }
}

std::string FirstText(void* widget, int depth) {
    if (!widget || !R::IsLive(widget) || depth > kMaxDepth) return {};
    if (R::ClassNameOf(widget) == L"TextBlock") return TextField(widget, L"Text");
    std::string found;
    ForEachChild(widget, [&](void* child) { if (found.empty()) found = FirstText(child, depth + 1); });
    return found;
}

void Walk(void* widget, Pane& pane, Category* into, int depth) {
    if (!widget || !R::IsLive(widget) || depth > kMaxDepth) return;
    const std::wstring cls = R::ClassNameOf(widget);
    if (cls == L"uicomp_gameRuleSlot_C") {
        if (!into) {  // a row outside every category: kept, under no name
            if (pane.categories.empty() || !pane.categories.back().name.empty()) pane.categories.emplace_back();
            into = &pane.categories.back();
        }
        into->rows.push_back(ReadRow(widget));
        return;
    }
    if (cls == L"ExpandableArea") {
        Category cat;
        cat.name = FirstText(ObjectField(widget, L"HeaderContent"), depth + 1);
        cat.hidden = IsHidden(widget);
        Walk(ObjectField(widget, L"BodyContent"), pane, &cat, depth + 1);
        pane.categories.push_back(std::move(cat));
        return;
    }
    ForEachChild(widget, [&](void* child) { Walk(child, pane, into, depth + 1); });
}

}  // namespace

bool IsUnlocked(const Row& row, const LockInputs& in) {
    if (!in.valid) return true;
    if (row.unlockAchievements.empty()) return in.maxDay >= row.unlockDay;
    for (const std::string& a : row.unlockAchievements) {
        if (std::find(in.achievements.begin(), in.achievements.end(), a) == in.achievements.end()) return false;
    }
    return true;
}

bool ReadLockInputs(LockInputs& out) {
    out = LockInputs{};
    void* gamemode = R::FindObjectByClass(ue_wrap::profile::name::GamemodeClass);
    if (!gamemode || !R::IsLive(gamemode)) return false;
    void* profile = ObjectField(gamemode, L"save_main");
    if (!profile || !R::IsLive(profile)) return false;
    out.maxDay = IntField(profile, L"maxDays", 0);
    // The menu also counts the listed slots' days; in a world, the running save's own day.
    int32_t hour = 0, minute = 0, day = 0;
    if (ue_wrap::daynightcycle::ReadSavedTime(hour, minute, day)) out.maxDay = std::max(out.maxDay, day);
    const int32_t achOff = R::FindPropertyOffset(R::ClassOf(profile), L"achievementsNames");
    if (achOff >= 0) {
        const auto* arr = reinterpret_cast<const NameArray*>(reinterpret_cast<uint8_t*>(profile) + achOff);
        for (int32_t i = 0; arr->data && i < arr->num && i < 1024; ++i)
            out.achievements.push_back(Narrow(R::ToString(arr->data[i])));
    }
    out.valid = true;
    return true;
}

bool Read(Pane& out) {
    out.valid = false;
    out.categories.clear();
    void* cls = R::FindClass(L"ui_gameRulesList_C");
    if (!cls) return false;
    void* tree = ObjectField(cls, L"WidgetTree");  // a property of the widget class object itself
    if (!tree || !R::IsLive(tree)) return false;
    Walk(ObjectField(tree, L"RootWidget"), out, nullptr, 0);
    for (const Category& c : out.categories) if (!c.rows.empty()) out.valid = true;
    // A walk that found categories but no rows is not a layout: an unusable pane hands back nothing,
    // so no caller can read categories out of a Read() that answered false.
    if (!out.valid) out.categories.clear();
    return out.valid;
}

}  // namespace ue_wrap::game_rules_pane
