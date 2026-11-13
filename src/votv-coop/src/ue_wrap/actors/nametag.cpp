// ue_wrap/actors/nametag.cpp -- see ue_wrap/actors/nametag.h.

#include "ue_wrap/actors/nametag.h"

#include "ue_wrap/actors/prop.h"          // IsDescendantOfProp
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"     // StringToFName
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::nametag {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kItemClass = L"prop_nametag_C";

// One FName field, by name on the instance's class; the offset cached per class.
struct Field {
    const wchar_t* name;
    void*          cls = nullptr;
    int32_t        off = -1;
    int32_t Of(void* obj) {
        void* c = R::ClassOf(obj);
        if (c != cls) { cls = c; off = R::FindPropertyOffset(c, name); }
        return off;
    }
};
Field g_tag{L"tag"};
Field g_nametag{L"nametag"};

bool ReadName(void* obj, Field& f, std::wstring& out) {
    const int32_t off = f.Of(obj);
    if (off < 0) return false;
    R::FName n{};
    std::memcpy(&n, static_cast<const uint8_t*>(obj) + off, sizeof(n));
    out = R::ToString(n);
    return true;
}

}  // namespace

bool IsNametagClass(void* cls) {
    for (void* c = cls; c; c = R::SuperStructOf(c))
        if (R::ToString(R::NameOf(c)) == kItemClass) return true;
    return false;
}

bool IsNametagItem(void* actor) {
    return actor && R::IsLive(actor) && IsNametagClass(R::ClassOf(actor));
}

bool ReadItemTag(void* item, std::wstring& out) {
    return IsNametagItem(item) && ReadName(item, g_tag, out);
}

bool ReadPropNametag(void* prop, std::wstring& out) {
    return prop && ue_wrap::prop::IsDescendantOfProp(prop) && ReadName(prop, g_nametag, out);
}

bool ApplyPropNametag(void* prop, const std::wstring& tag) {
    if (!prop || !R::IsLive(prop) || !ue_wrap::prop::IsDescendantOfProp(prop)) return false;
    const int32_t off = g_nametag.Of(prop);
    if (off < 0) return false;
    R::FName n{};
    if (!tag.empty() && tag != L"None") {
        n = ue_wrap::fname_utils::StringToFName(tag);
        if (n.ComparisonIndex == 0 && n.Number == 0) return false;   // the name could not be made
    }
    std::memcpy(static_cast<uint8_t*>(prop) + off, &n, sizeof(n));
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"setNametag");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(prop, f);
}

}  // namespace ue_wrap::nametag
