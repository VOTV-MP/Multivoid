// ue_wrap/world/profile.cpp -- see ue_wrap/world/profile.h.

#include "ue_wrap/world/profile.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

namespace ue_wrap::profile {
namespace {

namespace R = reflection;

// Each member is read from the live class once and latched either way: a class that lacks it -- a game
// update renamed it -- lacks it for the process, so the lookup never repeats and the miss is said once.
int32_t g_saveMainOff = -1;      // mainGamemode_C::save_main
bool    g_saveMainMissing = false;
// A member of save_main_C's `stats` struct, which a Blueprint-mangled name reaches only by its prefix.
struct Stat {
    const wchar_t* prefix;     // with its trailing underscore, so "days_" cannot match "days_total_"
    const wchar_t* what;       // for the one line said on a miss
    int32_t off = -1;          // stats' offset plus the member's
    bool    missing = false;
};
Stat g_daysTotal{L"days_total_", L"the days lived"};
Stat g_signalsFound{L"signals_found_", L"the signals found"};
Stat g_signalsProcessed{L"signals_processed_", L"the signals processed"};
int32_t g_keybindNamesOff = -1;  // save_main_C::keybindsNames, a TArray<FName>
int32_t g_keybindKeysOff = -1;   // save_main_C::keybinds_keys, a TArray<FString> in the same order
bool    g_keybindsMissing = false;
void*   g_progressFn = nullptr;  // save_main_C::progressAchievement
bool    g_progressMissing = false;

// The running world's profile, or null.
void* Profile() {
    void* gm = world_singleton::Gamemode();  // world-stamped: never a dying world's
    if (!gm || g_saveMainMissing) return nullptr;
    if (g_saveMainOff < 0) {
        g_saveMainOff = R::FindPropertyOffset(R::ClassOf(gm), L"save_main");
        if (g_saveMainOff < 0) {
            g_saveMainMissing = true;
            UE_LOGW("profile: mainGamemode_C has no save_main -- the profile is out of reach");
            return nullptr;
        }
    }
    void* p = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_saveMainOff);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// The address of `profile`'s `stat`, or null.
int32_t* StatOf(void* profile, Stat& stat) {
    if (!profile || stat.missing) return nullptr;
    if (stat.off < 0) {
        void* cls = R::ClassOf(profile);
        const int32_t statsOff = R::FindPropertyOffset(cls, L"stats");
        void* stats = statsOff >= 0 ? R::PropertyInnerStruct(cls, L"stats") : nullptr;
        const int32_t inner = stats ? R::FindPropertyOffsetByPrefix(stats, stat.prefix) : -1;
        if (inner < 0) {
            stat.missing = true;
            UE_LOGW("profile: save_main_C has no stats.%ls (stats@%d) -- %ls are out of reach", stat.prefix,
                    statsOff, stat.what);
            return nullptr;
        }
        stat.off = statsOff + inner;
    }
    return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(profile) + stat.off);
}

// The engine's TArray header: the data, then its count and capacity.
struct ArrayHeader {
    void*   data;
    int32_t num;
    int32_t max;
};

}  // namespace

bool ReadDaysTotal(int32_t& out) {
    const int32_t* v = StatOf(Profile(), g_daysTotal);
    if (!v) return false;
    out = *v;
    return true;
}

bool AddDaysTotal(int32_t n) {
    int32_t* v = StatOf(Profile(), g_daysTotal);
    if (!v) return false;
    *v += n;
    return true;
}

bool ReadSignalsFound(int32_t& out) {
    const int32_t* v = StatOf(Profile(), g_signalsFound);
    if (!v) return false;
    out = *v;
    return true;
}

bool AddSignalsFound(int32_t n) {
    int32_t* v = StatOf(Profile(), g_signalsFound);
    if (!v) return false;
    *v += n;
    return true;
}

bool ReadSignalsProcessed(int32_t& out) {
    const int32_t* v = StatOf(Profile(), g_signalsProcessed);
    if (!v) return false;
    out = *v;
    return true;
}

bool AddStat(std::wstring_view name, int32_t n) {
    Stat* stat = name == L"days_total"          ? &g_daysTotal
                 : name == L"signals_found"     ? &g_signalsFound
                 : name == L"signals_processed" ? &g_signalsProcessed
                                                : nullptr;
    int32_t* v = stat ? StatOf(Profile(), *stat) : nullptr;
    if (!v) return false;
    *v += n;
    return true;
}

bool KeybindDisplayName(const wchar_t* name, std::wstring& out) {
    void* p = Profile();
    if (!p || !name || g_keybindsMissing) return false;
    if (g_keybindNamesOff < 0 || g_keybindKeysOff < 0) {
        void* cls = R::ClassOf(p);
        g_keybindNamesOff = R::FindPropertyOffset(cls, L"keybindsNames");
        g_keybindKeysOff = R::FindPropertyOffset(cls, L"keybinds_keys");
        if (g_keybindNamesOff < 0 || g_keybindKeysOff < 0) {
            g_keybindsMissing = true;
            UE_LOGW("profile: save_main_C has no keybindsNames or keybinds_keys (%d/%d) -- a binding is out of reach",
                    g_keybindNamesOff, g_keybindKeysOff);
            return false;
        }
    }
    const auto* names = reinterpret_cast<const ArrayHeader*>(static_cast<uint8_t*>(p) + g_keybindNamesOff);
    const auto* keys = reinterpret_cast<const ArrayHeader*>(static_cast<uint8_t*>(p) + g_keybindKeysOff);
    if (!names->data || !keys->data || names->num <= 0 || names->num != keys->num) return false;
    const auto* nameAt = static_cast<const R::FName*>(names->data);
    for (int32_t i = 0; i < names->num; ++i) {
        if (!R::NameEquals(nameAt[i], name)) continue;
        const ArrayHeader& key = static_cast<const ArrayHeader*>(keys->data)[i];
        if (!key.data || key.num <= 1) return false;
        out.assign(static_cast<const wchar_t*>(key.data), static_cast<size_t>(key.num - 1));
        return true;
    }
    return false;
}

bool ProgressAchievement(const wchar_t* name, bool autosave) {
    void* p = Profile();
    if (!p || g_progressMissing) return false;
    if (!g_progressFn) {
        g_progressFn = R::FindFunction(R::ClassOf(p), L"progressAchievement");
        if (!g_progressFn) {
            g_progressMissing = true;
            UE_LOGW("profile: save_main_C has no progressAchievement -- an achievement cannot progress");
            return false;
        }
    }
    const R::FName achievement = fname_utils::StringToFName(name);
    if (achievement.ComparisonIndex == 0) return false;
    ParamFrame f(g_progressFn);
    const bool popup = true;
    if (!f.valid() || !f.Set(L"achievement", achievement) || !f.Set(L"popup", popup) ||
        !f.Set(L"autosave", autosave))
        return false;
    return Call(p, f);
}

}  // namespace ue_wrap::profile
