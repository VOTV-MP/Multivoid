// ue_wrap/actors/snack.cpp -- see ue_wrap/actors/snack.h.

#include "ue_wrap/actors/snack.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/votv_lib.h"

#include <cstring>
#include <vector>

namespace ue_wrap::snack {
namespace {

namespace R = ue_wrap::reflection;

// A class by name, found again when a world reload has re-created it.
void* ResolveClass(ue_wrap::CachedObjRef& ref, const wchar_t* name) {
    if (ref.Alive()) return ref.Raw();
    ref.Set(ue_wrap::object_index::ClassByName(name));
    return ref.Raw();
}

ue_wrap::CachedObjRef g_snackClass;
ue_wrap::CachedObjRef g_beerClass;

bool Walks(void* actor, ue_wrap::CachedObjRef& ref, const wchar_t* name) {
    if (!actor || !R::IsLive(actor)) return false;
    void* const base = ResolveClass(ref, name);
    return base && ue_wrap::prop::WalksToBase(R::ClassOf(actor), base);
}

// The beer's `open`, a bitfield bool read from the class of the instance asked, as a class a world reload re-created
// at the same address is asked again.
struct OpenField {
    ue_wrap::CachedObjRef cls;
    int32_t off = -1;
    uint8_t mask = 0;
    bool ok = false;
};
OpenField g_open;

bool ReadOpen(void* actor, bool& out) {
    void* const cls = R::ClassOf(actor);
    if (!cls) return false;
    if (!g_open.cls.Is(cls)) {
        g_open.cls.Set(cls);
        g_open.ok = R::FindBoolProperty(cls, L"open", g_open.off, g_open.mask);
    }
    if (!g_open.ok) return false;
    out = (*(reinterpret_cast<const uint8_t*>(actor) + g_open.off) & g_open.mask) != 0;
    return true;
}

}  // namespace

bool IsSnack(void* actor) { return Walks(actor, g_snackClass, L"prop_snack_C"); }

bool IsBeerFamily(void* actor) { return Walks(actor, g_beerClass, L"prop_snack_beer_C"); }

bool ReadSnackState(void* actor, int32_t& uses, bool& open) {
    static R::InstanceOffset sUses{L"uses"};
    if (!IsSnack(actor)) return false;
    const int32_t o = sUses.Of(actor);
    if (o < 0) return false;
    int32_t u = 0;
    std::memcpy(&u, reinterpret_cast<const uint8_t*>(actor) + o, sizeof(u));
    bool opened = false;
    if (IsBeerFamily(actor) && !ReadOpen(actor, opened)) return false;
    uses = u;
    open = opened;
    return true;
}

bool PatchSnackRecord(void* actor, int32_t uses, bool open) {
    if (!IsSnack(actor)) return false;
    const bool beer = IsBeerFamily(actor);
    ue_wrap::save_record::SaveRecord rec;
    if (!ue_wrap::save_record::CaptureOwnRecord(actor, rec)) return false;
    // prop_snack_C::getData files `uses` as ints[0][0]; the beer's getData files `open` as bools[1][0], behind the
    // base's own group. A record of another shape is not ours.
    if (rec.ints.empty() || rec.ints[0].empty()) return false;
    rec.ints[0][0] = uses;
    if (beer) {
        if (rec.bools.size() < 2 || rec.bools[1].empty()) return false;
        rec.bools[1][0] = open ? 1 : 0;
    }
    // The record is the actor's own, read above and patched in two fields, so every class it names is one this actor
    // already holds: nothing to load (LookupOnly).
    return ue_wrap::save_record::ApplyRecord(actor, rec, ue_wrap::save_record::ClassLoad::LookupOnly);
}

namespace {

// A parameterless Blueprint function of `obj`.
bool CallNoArgs(void* obj, const wchar_t* name) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(obj), name);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

bool ReadFName(void* obj, R::InstanceOffset& field, R::FName* out) {
    const int32_t o = field.Of(obj);
    if (o < 0) return false;
    std::memcpy(out, reinterpret_cast<const uint8_t*>(obj) + o, sizeof(*out));
    return true;
}

// `name` is written before the finish on the plain and food paths, as SetNamePropertyByName does in the script.
void* BeginWrapper(void* cls, const FVector& at, const FRotator& rot, uint8_t collision, R::FName name,
                   const std::vector<uint8_t>* foodData) {
    void* const actor = ue_wrap::engine::BeginDeferredSpawn(cls, at, rot, collision);
    if (!actor) return nullptr;
    static R::InstanceOffset sFood{L"foodData"};
    if (foodData) {
        const int32_t o = sFood.Of(actor);
        if (o >= 0) std::memcpy(reinterpret_cast<uint8_t*>(actor) + o, foodData->data(), foodData->size());
        else UE_LOGW("snack: the wrapper's foodData could not be written");
    }
    if (name.ComparisonIndex != 0 && !ue_wrap::prop::WritePropName(actor, name))
        UE_LOGW("snack: the wrapper's name could not be written before its finish");
    return actor;
}

}  // namespace

void* BirthLastUseWrapper(void* snack) {
    static R::InstanceOffset sTrash{L"trash"};
    namespace E = ue_wrap::engine;
    R::FName trash{};
    FVector at{};
    FRotator rot{};
    int32_t boolOff = -1;
    uint8_t mask = 0;
    if (!IsSnack(snack) || !ReadFName(snack, sTrash, &trash) || !E::TryGetActorLocation(snack, at) ||
        !E::TryGetActorRotation(snack, rot) || !R::FindBoolProperty(R::ClassOf(snack), L"spawnAsObject", boolOff, mask))
        return nullptr;
    const bool asObject = (*(reinterpret_cast<const uint8_t*>(snack) + boolOff) & mask) != 0;
    constexpr uint8_t kDefault = 0, kAdjust = 2;
    void* const propClass = ue_wrap::object_index::ClassByName(L"prop_C");
    void* actor = nullptr;
    bool stamp = false;
    if (!asObject) {
        actor = propClass ? BeginWrapper(propClass, at, rot, kDefault, trash, nullptr) : nullptr;
    } else {
        ue_wrap::votv_lib::PropObject obj;
        if (!ue_wrap::votv_lib::PropToObject(trash, snack, obj)) return nullptr;
        if (obj.objectClass) {
            actor = BeginWrapper(obj.objectClass, at, rot, kAdjust, R::FName{0, 0}, nullptr);
            stamp = obj.parseNameToObject;
        } else if (obj.isFood) {
            void* const foodClass = ue_wrap::object_index::ClassByName(L"prop_food_C");
            actor = foodClass ? BeginWrapper(foodClass, at, rot, kAdjust, trash, &obj.foodData) : nullptr;
        } else {
            actor = propClass ? BeginWrapper(propClass, at, rot, kAdjust, trash, nullptr) : nullptr;
        }
    }
    if (!actor) return nullptr;
    if (!E::FinishDeferredSpawn(actor, at, rot)) {
        E::DestroyActor(actor);
        return nullptr;
    }
    // The object path stamps the finished prop's name and runs its init(), as the script does through asProp (a prop's
    // own, which returns itself).
    if (stamp && ue_wrap::prop::IsDescendantOfProp(actor)) {
        if (!ue_wrap::prop::WritePropName(actor, trash) || !CallNoArgs(actor, L"init"))
            UE_LOGW("snack: the wrapper's name stamp or init() did not run");
    }
    return actor;
}

}  // namespace ue_wrap::snack
