// ue_wrap/devices/coffee.cpp -- see ue_wrap/devices/coffee.h.

#include "ue_wrap/devices/coffee.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::coffee {
namespace {

namespace R = reflection;

// A class as this world holds it, looked up by name again once its slot or serial no longer holds the
// kept one (a Blueprint class can be a new object in a new world).
struct Cls {
    const wchar_t* name;
    ue_wrap::CachedObjRef ref;
};
Cls g_vending{L"prop_coffeevending_C", {}};
Cls g_maker{L"prop_coffeeMaker_C", {}};
Cls g_bag{L"prop_coffeeBag_C", {}};

void* ClassFor(Cls& c) {
    if (void* k = c.ref.Get()) return k;
    void* k = ue_wrap::object_index::ClassByName(c.name);
    if (k) c.ref.Set(k);
    return k;
}

// A Blueprint bool is an FBoolProperty: its byte offset and the bit inside that byte, since bools can pack
// several to a byte and a raw byte read would read a neighbour's bit as this flag's. Found once per class
// object, again when the maker's class is a new object (a new world).
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
};
BoolField g_hasCup{L"hasCup"};
BoolField g_hasCoffee{L"hasCoffee"};
BoolField g_coffeeReady{L"coffeeReady"};
R::InstanceOffset g_amounts{L"amounts"};

bool ReadBool(void* obj, void* cls, BoolField& f, bool& out) {
    if (f.cls != cls) {
        f.cls = cls;
        f.off = -1;
        if (!R::FindBoolProperty(cls, f.name, f.off, f.mask)) f.off = -1;
    }
    if (f.off < 0) return false;
    out = (*(reinterpret_cast<const uint8_t*>(obj) + f.off) & f.mask) != 0;
    return true;
}

template <class T>
bool ReadField(void* obj, R::InstanceOffset& off, T& out) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    out = *reinterpret_cast<const T*>(reinterpret_cast<const char*>(obj) + o);
    return true;
}

}  // namespace

Device DeviceOf(void* actor) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return Device::None;
    if (cls == ClassFor(g_vending)) return Device::Vending;
    if (cls == ClassFor(g_maker)) return Device::Maker;
    return Device::None;
}

bool IsBag(void* actor) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    return cls && cls == ClassFor(g_bag);
}

bool ReadMakerFlags(void* maker, MakerFlags& out) {
    if (DeviceOf(maker) != Device::Maker) return false;
    void* const cls = R::ClassOf(maker);
    MakerFlags f;
    if (!ReadBool(maker, cls, g_hasCup, f.hasCup) || !ReadBool(maker, cls, g_hasCoffee, f.hasCoffee) ||
        !ReadBool(maker, cls, g_coffeeReady, f.coffeeReady)) return false;
    out = f;
    return true;
}

bool ReadBagAmounts(void* bag, int32_t& out) {
    return IsBag(bag) && ReadField(bag, g_amounts, out);
}

}  // namespace ue_wrap::coffee
