#include "coop/props/wire_class.h"

#include "coop/net/wire_class_path.h"
#include "ue_wrap/core/class_load.h"

namespace coop::wire_class {

void FillFromClass(coop::net::WireClassName& cn, const std::wstring& leaf, void* cls) {
    coop::net::wire_class_path::Fill(cn, leaf, ue_wrap::class_load::ObjectPathOfClass(cls));
}

void* Resolve(const coop::net::WireClassName& cn, Lookup lookup) {
    const std::wstring leaf = coop::net::wire_class_path::Leaf(cn);
    if (leaf.empty()) return nullptr;
    const std::wstring path = coop::net::wire_class_path::ObjectPath(cn);
    return ue_wrap::class_load::Resolve(leaf.c_str(), path.empty() ? nullptr : path.c_str(), lookup);
}

}  // namespace coop::wire_class
