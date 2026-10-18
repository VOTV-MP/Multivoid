// ue_wrap/actors/zombie.cpp -- see ue_wrap/actors/zombie.h.

#include "ue_wrap/actors/zombie.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ue_wrap::zombie {
namespace {

namespace P = profile;
namespace R = reflection;

// The AnimBP the family's BeginPlay casts its mesh's animation instance to.
constexpr const wchar_t* kAnimClass = L"kerfurOmegaV1_Skeleton_AnimBlueprint_Child_C";

R::InstanceOffset g_anim{L"anim"};

// A live object in the pointer slot `off` bytes into `base`, or null.
void* LiveAt(void* base, size_t off) {
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Says a step failed, once per step for this zombie's class (keyed by the class's name, which a world change
// does not move; the family has a dozen classes).
void Failed(void* zombie, const char* step) {
    struct Seen {
        R::FName    cls;
        const char* step;
    };
    static Seen seen[32];
    static int count = 0;
    const R::FName& name = R::NameOf(R::ClassOf(zombie));
    for (int i = 0; i < count; ++i)
        if (seen[i].cls.ComparisonIndex == name.ComparisonIndex && seen[i].cls.Number == name.Number &&
            std::strcmp(seen[i].step, step) == 0)
            return;
    if (count < 32) seen[count++] = Seen{name, step};
    UE_LOGW("zombie: %ls mirror presentation step '%s' did not run", R::ClassNameOf(zombie).c_str(), step);
}

// The `boney` flag's byte and bit mask, resolved once per class (a class without it is held too, as byte -1): the
// family has a dozen classes and a swing at each asks again.
struct BoolSlot {
    int32_t byteOff = -1;
    uint8_t mask = 0;
};

bool IsBoney(void* obj) {
    static ObjKeyedCache<BoolSlot, 16> sSlots;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    const BoolSlot* held = sSlots.Find(cls);
    if (!held) {
        BoolSlot slot;
        if (!R::FindBoolProperty(cls, L"boney", slot.byteOff, slot.mask) || slot.byteOff < 0) slot = BoolSlot{};
        held = &sSlots.Insert(cls, slot);
    }
    return held->byteOff >= 0 && (*(static_cast<const uint8_t*>(obj) + held->byteOff) & held->mask) != 0;
}

// The `headArmor` float's offset, resolved once per class like `boney` (-1 where a class lacks it).
int32_t HeadArmorOffset(void* obj) {
    static ObjKeyedCache<int32_t, 16> sOffsets;
    void* cls = R::ClassOf(obj);
    if (!cls) return -1;
    const int32_t* held = sOffsets.Find(cls);
    if (!held) held = &sOffsets.Insert(cls, R::FindPropertyOffset(cls, L"headArmor"));
    return *held;
}

}  // namespace

bool ApplyBeginPlayPresentation(void* zombie) {
    if (!zombie || !R::IsLive(zombie)) return false;
    void* mesh = LiveAt(zombie, P::off::ACharacter_Mesh);
    void* anim = mesh ? LiveAt(mesh, P::off::USkeletalMesh_AnimScriptInstance) : nullptr;
    void* base = object_index::ClassByName(kAnimClass);
    void* animCls = anim ? R::ClassOf(anim) : nullptr;
    // The cast fails in the game when the mesh runs no animation blueprint of the family's class: nothing to do.
    if (!animCls || !base || !R::IsDescendantOfAny(animCls, &base, 1)) return true;

    const int32_t off = g_anim.Of(zombie);
    if (off < 0) {
        Failed(zombie, "anim");
        return false;
    }
    *reinterpret_cast<void**>(static_cast<uint8_t*>(zombie) + off) = anim;

    void* fn = R::FindDispatchFunctionCached(R::ClassOf(zombie), L"setRandomAnimation");
    ParamFrame f(fn);
    if (!fn || !f.valid() || !Call(zombie, f)) {
        Failed(zombie, "setRandomAnimation");
        return false;
    }
    return true;
}

const wchar_t* PlayHitFeedback(void* zombie, const FVector& hitLoc) {
    if (!zombie || !R::IsLive(zombie)) return nullptr;
    const int32_t armorOff = HeadArmorOffset(zombie);
    const bool armored = armorOff >= 0 &&
                         *reinterpret_cast<const float*>(static_cast<const uint8_t*>(zombie) + armorOff) > 0.f;
    const wchar_t* name = armored ? L"hitHeadArmor" : IsBoney(zombie) ? L"hitBoney" : L"hitFlesh";
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(zombie), name);
    ParamFrame f(fn);
    if (!fn || !f.valid()) return nullptr;
    f.Set<FVector>(L"InputPin", hitLoc);
    return Call(zombie, f) ? name : nullptr;
}

}  // namespace ue_wrap::zombie
