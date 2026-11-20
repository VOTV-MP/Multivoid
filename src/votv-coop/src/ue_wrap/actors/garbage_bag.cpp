// ue_wrap/actors/garbage_bag.cpp -- see ue_wrap/actors/garbage_bag.h.

#include "ue_wrap/actors/garbage_bag.h"

#include "ue_wrap/actors/item_spawn.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::garbage_bag {
namespace {

namespace R = ue_wrap::reflection;

// Each class is cached through a CachedObjRef, as the prop base is: a level reload can destroy and
// re-create a UClass at the same address, and the liveness check covers that.
ue_wrap::CachedObjRef g_foldCls;
ue_wrap::CachedObjRef g_rollCls;
ue_wrap::CachedObjRef g_filledCls;

void* Cached(ue_wrap::CachedObjRef& slot, const wchar_t* name) {
    if (slot.Alive()) return slot.Raw();
    slot.Set(R::FindClass(name));
    return slot.Raw();
}

bool IsOfClass(void* obj, void* cls) {
    if (!obj || !cls) return false;
    void* objCls = R::ClassOf(obj);
    void* bases[1] = {cls};
    return objCls && R::IsDescendantOfAny(objCls, bases, 1);
}

// The roll's remaining-bag count; -1 until a roll has been seen.
int32_t g_bagsOff = -1;

}  // namespace

void* FoldClass() { return Cached(g_foldCls, L"prop_garbBagFold_C"); }

void* RollClass() { return Cached(g_rollCls, L"prop_garbBagRoll_C"); }

bool IsFold(void* obj) { return IsOfClass(obj, FoldClass()); }

bool IsRoll(void* obj) { return IsOfClass(obj, RollClass()); }

void* FilledClass() {
    if (!g_filledCls.Alive()) g_filledCls.Set(item_spawn::LoadedClass(L"prop_garbageBag_C"));
    return g_filledCls.Raw();
}

bool ReadRollCount(void* roll, int32_t& bags) {
    if (!IsRoll(roll)) return false;
    if (g_bagsOff < 0) g_bagsOff = R::FindPropertyOffset(R::ClassOf(roll), L"bags");
    if (g_bagsOff < 0) return false;
    bags = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(roll) + g_bagsOff);
    return true;
}

bool WriteRollCount(void* roll, int32_t bags) {
    int32_t cur = 0;
    if (!ReadRollCount(roll, cur)) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(roll) + g_bagsOff) = bags;
    return true;
}

}  // namespace ue_wrap::garbage_bag
