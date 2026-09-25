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
int32_t g_daysTotalOff = -1;     // save_main_C::stats plus the struct's days_total_<GUID>
bool    g_daysTotalMissing = false;
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

// The address of `profile`'s days_total, or null. The member sits in the `stats` struct under a
// Blueprint-mangled name, so it is found by its prefix.
int32_t* DaysTotalOf(void* profile) {
    if (!profile || g_daysTotalMissing) return nullptr;
    if (g_daysTotalOff < 0) {
        void* cls = R::ClassOf(profile);
        const int32_t statsOff = R::FindPropertyOffset(cls, L"stats");
        void* stats = statsOff >= 0 ? R::PropertyInnerStruct(cls, L"stats") : nullptr;
        const int32_t inner = stats ? R::FindPropertyOffsetByPrefix(stats, L"days_total_") : -1;
        if (inner < 0) {
            g_daysTotalMissing = true;
            UE_LOGW("profile: save_main_C has no stats.days_total (stats@%d) -- the days lived are out of reach",
                    statsOff);
            return nullptr;
        }
        g_daysTotalOff = statsOff + inner;
    }
    return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(profile) + g_daysTotalOff);
}

}  // namespace

bool ReadDaysTotal(int32_t& out) {
    const int32_t* v = DaysTotalOf(Profile());
    if (!v) return false;
    out = *v;
    return true;
}

bool AddDaysTotal(int32_t n) {
    int32_t* v = DaysTotalOf(Profile());
    if (!v) return false;
    *v += n;
    return true;
}

bool ProgressAchievement(const wchar_t* name) {
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
    const bool popup = true, autosave = false;
    if (!f.valid() || !f.Set(L"achievement", achievement) || !f.Set(L"popup", popup) ||
        !f.Set(L"autosave", autosave))
        return false;
    return Call(p, f);
}

}  // namespace ue_wrap::profile
