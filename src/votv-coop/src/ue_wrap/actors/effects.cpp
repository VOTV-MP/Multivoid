// ue_wrap/actors/effects.cpp -- see ue_wrap/actors/effects.h.

#include "ue_wrap/actors/effects.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>

namespace ue_wrap::effects {
namespace {

namespace R  = ue_wrap::reflection;
namespace DT = ue_wrap::data_table;
namespace SR = ue_wrap::save_record;

// ---- the names: the game's own table, read once the table has loaded -------------------------

std::vector<std::wstring> g_names;
bool g_haveNames = false;
bool g_saidAbsent = false;  // log-once latches only: neither one decides a result
bool g_saidRows = false;

// A table not loaded yet, or not yet readable, is not a verdict; each costs one walk of the object
// array a call until it reads. Our warning prints once, but data_table::Rows logs its own error at
// each retry.
bool ReadNamesOnce() {
    void* const table = R::FindObject(L"list_effects", L"DataTable");
    if (!table) {
        if (!g_saidAbsent) UE_LOGW("effects: list_effects is not loaded -- asked again at the next call");
        g_saidAbsent = true;
        return false;
    }
    std::vector<DT::RowRef> rows;
    if (!DT::Rows(table, rows, "effects")) {
        if (!g_saidRows) UE_LOGW("effects: list_effects' rows did not read -- asked again at the next call");
        g_saidRows = true;
        return false;
    }
    g_names.clear();
    g_names.reserve(rows.size());
    for (const DT::RowRef& r : rows) g_names.push_back(R::ToString(r.key));
    UE_LOGI("effects: list_effects holds %zu rows", g_names.size());
    return true;
}

// The table's own spelling of `name`, matched without case as an FName compares. False when the
// table is not loaded or holds no such row.
bool TableSpelling(const std::wstring& name, std::wstring* out) {
    std::vector<std::wstring> names;
    if (!Names(&names)) return false;
    for (const std::wstring& n : names) {
        if (_wcsicmp(n.c_str(), name.c_str()) != 0) continue;
        *out = n;
        return true;
    }
    return false;
}

// StringToFName returns NAME_None on a failed conversion; a table spelling is never empty.
bool IsNone(const R::FName& n) { return n.ComparisonIndex == 0 && n.Number == 0; }

// ---- the gamemode's arrays and verbs, resolved per gamemode class ----------------------------

struct Gamemode {
    void*   cls = nullptr;
    int32_t effectsNames = -1;   // TArray<FName>
    int32_t effects = -1;        // TArray<effect_C*>
    void*   addEffectFn = nullptr;
    void*   removeEffectFn = nullptr;
};
Gamemode g_gm;

// The running world's gamemode with its arrays and verbs resolved for its class, the vitals shape.
// A name that does not resolve stays unresolved for that class and is logged once.
void* GamemodeObject() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (void* const cls = R::ClassOf(gm); cls != g_gm.cls) {
        g_gm = Gamemode{};
        g_gm.cls = cls;
        g_gm.effectsNames = R::FindPropertyOffset(cls, L"effects_names");
        g_gm.effects = R::FindPropertyOffset(cls, L"effects");
        g_gm.addEffectFn = R::FindFunction(cls, L"addEffect");
        g_gm.removeEffectFn = R::FindFunction(cls, L"removeEffect");
        const std::wstring label = R::ToString(R::NameOf(cls));
        if (g_gm.effectsNames < 0) UE_LOGE("effects: %ls.effects_names did not resolve", label.c_str());
        if (g_gm.effects < 0) UE_LOGE("effects: %ls.effects did not resolve", label.c_str());
        if (!g_gm.addEffectFn) UE_LOGE("effects: %ls.addEffect did not resolve", label.c_str());
        if (!g_gm.removeEffectFn) UE_LOGE("effects: %ls.removeEffect did not resolve", label.c_str());
    }
    return gm;
}

// `strength` and `time` sit on effect_C and are inherited by the eight child classes, so one
// resolution on the parent serves every actor. Asked again only while the class is not loaded.
int32_t g_strengthOff = -1;
int32_t g_timeOff = -1;
bool    g_effectLooked = false;

bool EnsureEffectOffsets() {
    if (!g_effectLooked) {
        void* const cls = R::FindClass(L"effect_C");
        if (!cls) return false;
        g_effectLooked = true;
        g_strengthOff = R::FindPropertyOffset(cls, L"strength");
        g_timeOff = R::FindPropertyOffset(cls, L"time");
        if (g_strengthOff < 0) UE_LOGE("effects: effect_C.strength did not resolve");
        if (g_timeOff < 0) UE_LOGE("effects: effect_C.time did not resolve");
    }
    return g_strengthOff >= 0 && g_timeOff >= 0;
}

// ---- the save object's reset, resolved per save class ----------------------------------------

struct SaveSlot {
    void*   cls = nullptr;
    int32_t objectsData = -1;   // TArray<Fstruct_save>
    int32_t gamemode = -1;      // UmainGamemode_C*, the hint's target
    void*   resetFn = nullptr;  // reset_player_effects
};
SaveSlot g_slot;

// `slot`'s class with the reset verb and its two rows resolved, the gamemode shape. A name that
// does not resolve stays unresolved for that class and is logged once.
const SaveSlot& SaveSlotFor(void* slot) {
    if (void* const cls = R::ClassOf(slot); cls != g_slot.cls) {
        g_slot = SaveSlot{};
        g_slot.cls = cls;
        g_slot.objectsData = R::FindPropertyOffset(cls, L"objectsData");
        g_slot.gamemode = R::FindPropertyOffset(cls, L"gamemode");
        g_slot.resetFn = R::FindFunction(cls, L"reset_player_effects");
        const std::wstring label = R::ToString(R::NameOf(cls));
        if (g_slot.objectsData < 0) UE_LOGE("effects: %ls.objectsData did not resolve", label.c_str());
        if (g_slot.gamemode < 0) UE_LOGE("effects: %ls.gamemode did not resolve", label.c_str());
        if (!g_slot.resetFn) UE_LOGE("effects: %ls.reset_player_effects did not resolve", label.c_str());
    }
    return g_slot;
}

// The array's length, false when its header does not read: save_record rejects a garbage header to
// an empty one, which a raw Num then tells from a true empty.
bool ArrayLength(const void* base, int32_t off, int32_t* num) {
    const SR::Arr a = SR::ReadArr(base, off);
    int32_t raw = 0;
    std::memcpy(&raw, static_cast<const uint8_t*>(base) + off + 8, sizeof(raw));
    if (a.num != raw) return false;
    *num = a.num;
    return true;
}

}  // namespace

int ResetOnSaveObject(void* saveSlot, bool* gamemodeSet) {
    if (gamemodeSet) *gamemodeSet = false;
    if (!SR::PlausibleObjPtr(saveSlot) || !R::IsLive(saveSlot)) return -1;
    const SaveSlot& s = SaveSlotFor(saveSlot);
    if (s.objectsData < 0 || !s.resetFn) return -1;
    int32_t before = 0;
    if (!ArrayLength(saveSlot, s.objectsData, &before)) return -1;
    if (gamemodeSet && s.gamemode >= 0) {
        void* gm = nullptr;
        std::memcpy(&gm, static_cast<const uint8_t*>(saveSlot) + s.gamemode, sizeof(gm));
        *gamemodeSet = gm != nullptr;
    }
    ParamFrame f(s.resetFn);
    if (!f.valid() || !Call(saveSlot, f)) return -1;
    int32_t after = 0;
    if (!ArrayLength(saveSlot, s.objectsData, &after)) return -1;
    return before - after;
}

bool Names(std::vector<std::wstring>* out) {
    if (!out) return false;
    if (!g_haveNames) g_haveNames = ReadNamesOnce();
    if (!g_haveNames) return false;
    *out = g_names;
    return true;
}

bool List(std::vector<Entry>* out) {
    if (!out) return false;
    void* const gm = GamemodeObject();
    if (!gm || g_gm.effectsNames < 0 || g_gm.effects < 0) return false;
    const SR::Arr names = SR::ReadArr(gm, g_gm.effectsNames);
    const SR::Arr actors = SR::ReadArr(gm, g_gm.effects);
    if (names.num != actors.num) return false;
    // effect_C is needed only to read an actor: an empty pair of arrays lists as empty without it.
    if (actors.num > 0 && !EnsureEffectOffsets()) return false;
    out->clear();
    out->reserve(static_cast<size_t>(names.num));
    for (int32_t i = 0; i < names.num; ++i) {
        R::FName n{};
        void* a = nullptr;
        std::memcpy(&n, names.data + static_cast<size_t>(i) * sizeof(R::FName), sizeof(n));
        std::memcpy(&a, actors.data + static_cast<size_t>(i) * sizeof(void*), sizeof(a));
        Entry e{R::ToString(n), false, 0.f, 0.f};
        if (SR::PlausibleObjPtr(a) && R::IsLive(a)) {
            e.live = true;
            std::memcpy(&e.strength, static_cast<const uint8_t*>(a) + g_strengthOff, sizeof(float));
            std::memcpy(&e.time, static_cast<const uint8_t*>(a) + g_timeOff, sizeof(float));
        }
        out->push_back(std::move(e));
    }
    return true;
}

size_t ForEachWorldEffect(void (*fn)(void* effect, void* ctx), void* ctx) {
    if (!fn) return 0;
    void* const gm = GamemodeObject();
    if (!gm || g_gm.effects < 0) return 0;
    int32_t length = 0;
    if (!ArrayLength(gm, g_gm.effects, &length)) return 0;
    const SR::Arr actors = SR::ReadArr(gm, g_gm.effects);
    size_t visited = 0;
    for (int32_t i = 0; i < actors.num; ++i) {
        void* a = nullptr;
        std::memcpy(&a, actors.data + static_cast<size_t>(i) * sizeof(void*), sizeof(a));
        if (!SR::PlausibleObjPtr(a) || !R::IsLive(a)) continue;
        fn(a, ctx);
        ++visited;
    }
    return visited;
}

bool Add(const std::wstring& name, float strength, float seconds) {
    if (!std::isfinite(strength) || !std::isfinite(seconds)) return false;
    std::wstring spelled;
    if (!TableSpelling(name, &spelled)) return false;
    void* const gm = GamemodeObject();
    if (!gm || !g_gm.addEffectFn) return false;
    const R::FName effect = fname_utils::StringToFName(spelled);
    if (IsNone(effect)) return false;
    ParamFrame f(g_gm.addEffectFn);
    if (!f.valid()) return false;
    if (!f.Set<R::FName>(L"effect", effect) || !f.Set<float>(L"strength", strength) ||
        !f.Set<float>(L"time", seconds) || !f.Set<bool>(L"incrementStrength", false) ||
        !f.Set<bool>(L"incrementTime", false))
        return false;
    return Call(gm, f);
}

bool Remove(const std::wstring& name) {
    std::wstring spelled;
    if (!TableSpelling(name, &spelled)) return false;
    std::vector<Entry> entries;
    if (!List(&entries)) return false;
    const Entry* first = nullptr;
    for (const Entry& e : entries) {
        if (_wcsicmp(e.name.c_str(), spelled.c_str()) != 0) continue;
        first = &e;
        break;
    }
    if (!first || !first->live) return false;
    void* const gm = GamemodeObject();
    if (!gm || !g_gm.removeEffectFn) return false;
    const R::FName pin = fname_utils::StringToFName(spelled);
    if (IsNone(pin)) return false;
    ParamFrame f(g_gm.removeEffectFn);
    if (!f.valid()) return false;
    if (!f.Set<R::FName>(L"InputPin", pin)) return false;
    return Call(gm, f);
}

}  // namespace ue_wrap::effects
