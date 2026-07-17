// ue_wrap/daily_task.cpp -- see ue_wrap/daily_task.h.

#include "ue_wrap/desk/daily_task.h"

#include "ue_wrap/world/economy.h"     // SaveSlotPtr (the one gamemode->saveSlot resolve)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::daily_task {
namespace {

namespace R = reflection;

// Measured struct-internal offsets (see the header's layout table).
constexpr int32_t kOffActive         = 0x00;
constexpr int32_t kOffSigRequired    = 0x08;
constexpr int32_t kOffSigCompleted   = 0x18;
constexpr int32_t kOffRequiredDishes = 0x28;
constexpr int32_t kOffRewardSig      = 0x38;
constexpr int32_t kOffRewardSat      = 0x3C;
constexpr int32_t kOffReelBig        = 0x40;
constexpr int32_t kOffReelSmall      = 0x44;

// Engine TArray header (UE4 x64): {T* data; int32 num; int32 max}.
struct TArrayRaw {
    void*   data;
    int32_t num;
    int32_t max;
};

int32_t g_offTaskNew = -1;  // saveSlot_C::taskNew (reflected; fallback 0x0CA8)

uint8_t* TaskNewBase() {
    void* save = ue_wrap::economy::SaveSlotPtr();
    if (!save) return nullptr;
    if (g_offTaskNew < 0) {
        g_offTaskNew = R::FindPropertyOffset(R::ClassOf(save), L"taskNew");
        if (g_offTaskNew < 0) {
            UE_LOGW("daily_task: taskNew offset not found -- using fallback 0x0CA8");
            g_offTaskNew = 0x0CA8;
        }
    }
    return reinterpret_cast<uint8_t*>(save) + g_offTaskNew;
}

TArrayRaw* ArrayAt(uint8_t* base, Which which) {
    const int32_t off = (which == Which::SigRequired)  ? kOffSigRequired
                      : (which == Which::SigCompleted) ? kOffSigCompleted
                                                        : kOffRequiredDishes;
    return reinterpret_cast<TArrayRaw*>(base + off);
}

// Defensive sanity for a raw engine array read (a garbage num would smash the wire fill).
bool SaneArray(const TArrayRaw* a) {
    if (!a) return false;
    if (a->num < 0 || a->num > 4096) return false;
    if (a->num > 0 && !a->data) return false;
    return true;
}

}  // namespace

bool Read(View& out) {
    uint8_t* base = TaskNewBase();
    if (!base) return false;
    const TArrayRaw* sr = ArrayAt(base, Which::SigRequired);
    const TArrayRaw* sc = ArrayAt(base, Which::SigCompleted);
    const TArrayRaw* rd = ArrayAt(base, Which::RequiredDishes);
    if (!SaneArray(sr) || !SaneArray(sc) || !SaneArray(rd)) return false;
    out.active    = *reinterpret_cast<const bool*>(base + kOffActive);
    out.rewardSig = *reinterpret_cast<const int32_t*>(base + kOffRewardSig);
    out.rewardSat = *reinterpret_cast<const int32_t*>(base + kOffRewardSat);
    out.reelBig   = *reinterpret_cast<const float*>(base + kOffReelBig);
    out.reelSmall = *reinterpret_cast<const float*>(base + kOffReelSmall);
    out.sigRequired       = static_cast<const int32_t*>(sr->data);
    out.sigRequiredNum    = sr->num;
    out.sigCompleted      = static_cast<const int32_t*>(sc->data);
    out.sigCompletedNum   = sc->num;
    out.requiredDishes    = static_cast<const int32_t*>(rd->data);
    out.requiredDishesNum = rd->num;
    return true;
}

bool WriteScalars(bool active, int32_t rewardSig, int32_t rewardSat,
                  float reelBig, float reelSmall) {
    uint8_t* base = TaskNewBase();
    if (!base) return false;
    *reinterpret_cast<bool*>(base + kOffActive)       = active;
    *reinterpret_cast<int32_t*>(base + kOffRewardSig) = rewardSig;
    *reinterpret_cast<int32_t*>(base + kOffRewardSat) = rewardSat;
    *reinterpret_cast<float*>(base + kOffReelBig)     = reelBig;
    *reinterpret_cast<float*>(base + kOffReelSmall)   = reelSmall;
    return true;
}

bool WriteArray(Which which, const int32_t* vals, int32_t count) {
    if (count < 0 || (count > 0 && !vals)) return false;
    uint8_t* base = TaskNewBase();
    if (!base) return false;
    TArrayRaw* a = ArrayAt(base, which);
    if (!SaneArray(a)) return false;
    if (a->num == count) {
        if (count > 0) std::memcpy(a->data, vals, sizeof(int32_t) * count);
        return true;
    }
    // Length changed (a fresh rollover mints new arrays): rebuild the engine
    // allocation (int32 POD; the inventory.cpp ApplyToSaveObject precedent).
    void* buf = nullptr;
    if (count > 0) {
        buf = R::EngineAlloc(sizeof(int32_t) * static_cast<size_t>(count));
        if (!buf) {
            UE_LOGW("daily_task: EngineAlloc(%d ints) failed -- array left unchanged",
                    count);
            return false;
        }
        std::memcpy(buf, vals, sizeof(int32_t) * count);
    }
    if (a->data) R::EngineFree(a->data);
    a->data = buf;
    a->num  = count;
    a->max  = count;
    return true;
}

}  // namespace ue_wrap::daily_task
