// ue_wrap/actors/snack.cpp -- see ue_wrap/actors/snack.h.

#include "ue_wrap/actors/snack.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstring>

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

}  // namespace ue_wrap::snack
