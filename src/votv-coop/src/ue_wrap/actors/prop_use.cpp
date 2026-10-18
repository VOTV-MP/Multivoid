// ue_wrap/actors/prop_use.cpp -- see ue_wrap/actors/prop_use.h.

#include "ue_wrap/actors/prop_use.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/hit_result.h"

#include <cstring>

namespace ue_wrap::prop_use {
namespace {

namespace R = ue_wrap::reflection;

// A bitfield bool read from the class of the instance it is asked with: the class is held by its slot and serial, so a
// class a world reload re-created at the same address is asked again, and a class that does not declare the field
// stays unreadable without a second walk.
struct BoolField {
    const wchar_t*      name;
    ue_wrap::CachedObjRef cls;
    int32_t             off = -1;
    uint8_t             mask = 0;
    bool                ok = false;
};

bool ReadBool(BoolField& f, void* obj, bool& out) {
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (!f.cls.Is(cls)) {
        f.cls.Set(cls);
        f.ok = R::FindBoolProperty(cls, f.name, f.off, f.mask);
    }
    if (!f.ok) return false;
    out = (*(reinterpret_cast<const uint8_t*>(obj) + f.off) & f.mask) != 0;
    return true;
}

template <class T>
bool ReadAt(R::InstanceOffset& off, void* obj, T& out) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    std::memcpy(&out, reinterpret_cast<const uint8_t*>(obj) + o, sizeof(T));
    return true;
}

}  // namespace

bool CallActionOptionIndex(void* prop, void* player, void* hitComponent, const FVector& hitLocation, uint8_t action,
                           void* lookAtComponent) {
    if (!prop || !R::IsLive(prop) || !player) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", prop, hitComponent, hitLocation) &&
           f.Set<uint8_t>(L"action", action) && f.Set<void*>(L"lookAtComponent", lookAtComponent) && Call(prop, f);
}

bool CallPlayerUsedOn(void* prop, void* player, void* hitComponent, const FVector& hitLocation, void* lookAtComponent,
                      void* holdObject, R::FName holdPropName) {
    if (!prop || !R::IsLive(prop) || !player) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"playerUsedOn");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", prop, hitComponent, hitLocation) &&
           f.Set<void*>(L"lookAtComponent", lookAtComponent) && f.Set<void*>(L"holdObject", holdObject) &&
           f.SetRaw(L"holdPropName", &holdPropName, static_cast<int32_t>(sizeof(holdPropName))) && Call(prop, f);
}

bool ReadFoodUse(void* prop, int32_t& uses, bool& notEdible) {
    static R::InstanceOffset sUses{L"uses"};
    static BoolField sNotEdible{L"notEdible", {}};
    if (!prop || !R::IsLive(prop)) return false;
    int32_t u = 0;
    bool n = false;
    if (!ReadAt(sUses, prop, u) || !ReadBool(sNotEdible, prop, n)) return false;
    uses = u;
    notEdible = n;
    return true;
}

bool ReadSoapAmount(void* soap, int32_t& amount) {
    static R::InstanceOffset sAmount{L"soapAmount"};
    return soap && R::IsLive(soap) && ReadAt(sAmount, soap, amount);
}

bool ReadSpongeSoap(void* sponge, float& amount) {
    static R::InstanceOffset sAmount{L"soapAmount"};
    return sponge && R::IsLive(sponge) && ReadAt(sAmount, sponge, amount);
}

}  // namespace ue_wrap::prop_use
