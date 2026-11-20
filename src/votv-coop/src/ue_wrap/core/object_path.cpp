// ue_wrap/core/object_path.cpp -- see ue_wrap/core/object_path.h.

#include "ue_wrap/core/object_path.h"

#include "ue_wrap/core/reflection.h"

namespace ue_wrap::object_path {

namespace R = ue_wrap::reflection;

std::wstring Of(void* o) {
    if (!o) return {};
    void* const outer = R::OuterOf(o);
    const std::wstring name = R::ToString(R::NameOf(o));
    if (!outer) return name;
    return Of(outer) + (R::OuterOf(outer) ? L":" : L".") + name;
}

}  // namespace ue_wrap::object_path
