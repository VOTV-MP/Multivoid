#include "coop/net/wire_class_path.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::net::wire_class_path {

namespace {

constexpr size_t kCap = sizeof(WireClassName::data);   // 63

// Whether `leaf` is a Blueprint leaf ("..._C") whose package is named for it.
bool EndsWithC(const std::wstring& s) { return s.size() > 2 && s.compare(s.size() - 2, 2, L"_C") == 0; }

}  // namespace

bool Fill(WireClassName& cn, const std::wstring& leaf, const std::wstring& objectPath) {
    std::memset(&cn, 0, sizeof(cn));
    size_t n = 0;
    for (size_t i = 0; i < leaf.size() && i < kCap; ++i) cn.data[n++] = static_cast<char>(leaf[i]);
    cn.len = static_cast<uint8_t>(n);
    if (objectPath.empty() || n != leaf.size()) return false;
    const size_t dot = objectPath.rfind(L'.');
    if (dot == std::wstring::npos || objectPath.compare(dot + 1, std::wstring::npos, leaf) != 0) return false;
    const std::wstring pkg = objectPath.substr(0, dot);
    const size_t slash = pkg.rfind(L'/');
    if (slash == std::wstring::npos || pkg.compare(0, 6, L"/Game/") != 0) return false;
    uint8_t flag = 0;
    std::wstring body;
    if (EndsWithC(leaf) && pkg.compare(slash + 1, std::wstring::npos, leaf.substr(0, leaf.size() - 2)) == 0) {
        body = pkg.substr(0, slash + 1);                  // the directory; the package is it plus the leaf without _C
    } else {
        body = pkg;
        flag = kFullPackage;
    }
    if (body.empty() || body.size() > 62 || n + 2 + body.size() > kCap) return false;
    for (wchar_t c : body)
        if (c <= 0 || c > 127) return false;
    cn.data[n] = 0;
    cn.data[n + 1] = static_cast<char>(flag | static_cast<uint8_t>(body.size()));
    for (size_t i = 0; i < body.size(); ++i) cn.data[n + 2 + i] = static_cast<char>(body[i]);
    return true;
}

std::wstring Leaf(const WireClassName& cn) {
    std::wstring s;
    const size_t n = cn.len < kCap ? cn.len : kCap;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(cn.data[i])));
    return s;
}

std::wstring ObjectPath(const WireClassName& cn) {
    const size_t n = cn.len;
    if (n == 0 || n + 2 > kCap) return {};
    if (cn.data[n] != 0) return {};
    const uint8_t t = static_cast<uint8_t>(cn.data[n + 1]);
    const size_t plen = t & 0x7F;
    if (plen == 0 || n + 2 + plen > kCap) return {};
    std::wstring body;
    for (size_t i = 0; i < plen; ++i) {
        const unsigned char c = static_cast<unsigned char>(cn.data[n + 2 + i]);
        if (c == 0 || c > 127) return {};
        body.push_back(static_cast<wchar_t>(c));
    }
    const std::wstring leaf = Leaf(cn);
    std::wstring pkg;
    if (t & kFullPackage) {
        pkg = body;
    } else {
        if (body.back() != L'/' || !EndsWithC(leaf)) return {};
        pkg = body + leaf.substr(0, leaf.size() - 2);
    }
    return pkg + L"." + leaf;
}

void RunSelftest() {
    int pass = 0, total = 0;
    const char* failed = nullptr;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        if (!failed) failed = what;
        UE_LOGE("wire_class_path selftest FAIL: %s", what);
    };

    WireClassName c{};
    // The common form: the package is named for the leaf, only the directory travels.
    check(Fill(c, L"prop_food_mre_AB_C", L"/Game/objects/prop_food_mre_AB.prop_food_mre_AB_C"), "dir form fills");
    check(Leaf(c) == L"prop_food_mre_AB_C" && c.len == 18, "dir form keeps the leaf and its length");
    check(ObjectPath(c) == L"/Game/objects/prop_food_mre_AB.prop_food_mre_AB_C", "dir form round-trips");
    check(c.data[c.len] == 0, "the leaf stays a C string");
    // A subdirectory, and a package named otherwise (the whole package travels).
    check(Fill(c, L"prop_x_C", L"/Game/objects/misc/prop_x.prop_x_C") &&
              ObjectPath(c) == L"/Game/objects/misc/prop_x.prop_x_C", "a subdirectory round-trips");
    check(Fill(c, L"thing_C", L"/Game/stuff/other_name.thing_C") &&
              ObjectPath(c) == L"/Game/stuff/other_name.thing_C" && (static_cast<uint8_t>(c.data[c.len + 1]) & kFullPackage),
          "a package named otherwise travels whole");
    // No path: an empty one, a native class, another object's name, a path that does not fit.
    check(!Fill(c, L"prop_x_C", L"") && ObjectPath(c).empty() && Leaf(c) == L"prop_x_C", "an empty path leaves the leaf alone");
    check(!Fill(c, L"Actor", L"/Script/Engine.Actor") && ObjectPath(c).empty(), "a native class carries no path");
    check(!Fill(c, L"prop_x_C", L"/Game/objects/prop_x.prop_y_C") && ObjectPath(c).empty(), "a path naming another object is left off");
    const std::wstring longLeaf = L"prop_a_very_long_class_name_that_nearly_fills_the_whole_field_C";   // 61
    check(!Fill(c, longLeaf, L"/Game/objects/" + longLeaf.substr(0, longLeaf.size() - 2) + L"." + longLeaf) &&
              Leaf(c) == longLeaf, "a path that does not fit is left off, the leaf kept");
    // An old peer's name: zeros after the leaf read as no path, whatever the leaf.
    WireClassName old{};
    old.len = 7;
    std::memcpy(old.data, "prop_C_", 7);
    check(ObjectPath(old).empty(), "an old peer's name has no path");
    // Hostile bytes never parse into a path.
    WireClassName h = c;
    Fill(h, L"prop_x_C", L"/Game/objects/prop_x.prop_x_C");
    h.data[h.len + 1] = static_cast<char>(0x7F);       // a length past the field
    check(ObjectPath(h).empty(), "a length past the field is no path");
    Fill(h, L"prop_x_C", L"/Game/objects/prop_x.prop_x_C");
    h.data[h.len] = 'x';                                 // the leaf not terminated
    check(ObjectPath(h).empty(), "an unterminated leaf is no path");
    Fill(h, L"prop_x_C", L"/Game/objects/prop_x.prop_x_C");
    h.data[h.len + 3] = 0;                               // a NUL inside the path
    check(ObjectPath(h).empty(), "a NUL inside the path is no path");
    Fill(h, L"prop_x_C", L"/Game/objects/prop_x.prop_x_C");
    h.data[h.len + 2 + 13] = 'x';                        // the directory not ending in a slash
    check(ObjectPath(h).empty(), "a directory without its slash is no path");
    check(!HasPath(WireClassName{}), "an empty name has no path");

    if (!failed) UE_LOGI("wire_class_path selftest: ALL PASS (%d checks)", total);
    else UE_LOGE("wire_class_path selftest FAIL: %s (%d of %d checks passed)", failed, pass, total);
}

}  // namespace coop::net::wire_class_path
