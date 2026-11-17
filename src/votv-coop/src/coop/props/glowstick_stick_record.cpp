// coop/props/glowstick_stick_record.cpp -- see coop/props/glowstick_stick_record.h.

#include "coop/props/glowstick_stick_record.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace coop::glowstick_stick_record {
namespace {

bool AllFinite(const float* f, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(f[i])) return false;
    return true;
}

}  // namespace

Read ReadStick(const ue_wrap::save_record::SaveRecord& rec, StickFields& out) {
    if (rec.vectors.empty() || rec.vectors[0].size() < 3) return Read::Malformed;
    if (rec.transforms.empty() || rec.transforms[0].empty()) return Read::Malformed;
    if (rec.names.size() < 2 || rec.names[1].empty()) return Read::Malformed;
    if (rec.bools.size() < 2 || rec.bools[1].size() < 2) return Read::Malformed;
    StickFields f;
    f.sticked = rec.bools[1][0] != 0;
    f.stickStatic = rec.bools[1][1] != 0;
    std::memcpy(f.loc, rec.vectors[0][0].data(), sizeof(f.loc));
    std::memcpy(f.rel1, rec.vectors[0][1].data(), sizeof(f.rel1));
    std::memcpy(f.rel2, rec.vectors[0][2].data(), sizeof(f.rel2));
    std::memcpy(f.xform, rec.transforms[0][0].data(), sizeof(f.xform));
    f.attachKey = rec.names[1][0];
    if (f.attachKey == L"None") f.attachKey.clear();
    if (!AllFinite(f.loc, 3) || !AllFinite(f.rel1, 3) || !AllFinite(f.rel2, 3) || !AllFinite(f.xform, 10))
        return Read::NonFinite;
    out = std::move(f);
    return Read::Ok;
}

bool Same(const StickFields& a, const StickFields& b) {
    return a.sticked == b.sticked && a.stickStatic == b.stickStatic && a.attachKey == b.attachKey &&
           std::memcmp(a.loc, b.loc, sizeof(a.loc)) == 0 && std::memcmp(a.rel1, b.rel1, sizeof(a.rel1)) == 0 &&
           std::memcmp(a.rel2, b.rel2, sizeof(a.rel2)) == 0 && std::memcmp(a.xform, b.xform, sizeof(a.xform)) == 0;
}

}  // namespace coop::glowstick_stick_record
