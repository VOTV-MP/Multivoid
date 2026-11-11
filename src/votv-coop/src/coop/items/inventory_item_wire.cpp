// coop/items/inventory_item_wire.cpp -- see coop/items/inventory_item_wire.h.

#include "coop/items/inventory_item_wire.h"

#include "coop/items/save_record_wire.h"

namespace coop::inventory_item_wire {
namespace {

namespace II = ue_wrap::inventory_item;
namespace W  = coop::save_record_wire;

bool Fail(const char** why, const char* text) {
    if (why) *why = text;
    return false;
}

}  // namespace

void SerItem(std::vector<uint8_t>& b, const II::Item& it) {
    W::AppWStr(b, it.classPath);
    for (float f : it.xform) W::AppF32(b, f);
    W::AppWStr(b, it.name);
    W::AppWStr(b, it.tag);
    W::AppWStr(b, it.key);
    W::AppWStr(b, it.attachKey);
    W::AppU32(b, static_cast<uint32_t>(it.actorData.size()));
    b.insert(b.end(), it.actorData.begin(), it.actorData.end());
    W::AppI32(b, it.index);
    W::AppF32(b, it.posX);
    W::AppF32(b, it.posY);
    W::AppF32(b, it.rotation);
    W::AppU8(b, it.compartment);
    W::AppU8(b, it.amount);
}

bool DeItem(const std::vector<uint8_t>& b, size_t& o, II::Item& it) {
    it = II::Item{};
    if (!W::RdWStr(b, o, it.classPath)) return false;
    for (float& f : it.xform)
        if (!W::RdF32(b, o, f)) return false;
    uint32_t n = 0;
    if (!W::RdWStr(b, o, it.name) || !W::RdWStr(b, o, it.tag) || !W::RdWStr(b, o, it.key) ||
        !W::RdWStr(b, o, it.attachKey) || !W::RdU32(b, o, n) || n > II::kMaxActorData || !W::Feasible(n, b, o))
        return false;
    it.actorData.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
    o += n;
    return W::RdI32(b, o, it.index) && W::RdF32(b, o, it.posX) && W::RdF32(b, o, it.posY) &&
           W::RdF32(b, o, it.rotation) && W::RdU8(b, o, it.compartment) && W::RdU8(b, o, it.amount);
}

std::vector<uint8_t> ItemWire(const II::Item& it) {
    std::vector<uint8_t> b;
    SerItem(b, it);
    return b;
}

size_t ItemBytes(const II::Item& it) { return ItemWire(it).size(); }

bool AppItem(std::vector<uint8_t>& b, const II::Item& it) {
    const std::vector<uint8_t> w = ItemWire(it);
    if (w.size() > kMaxItemBytes) return false;
    W::AppU32(b, static_cast<uint32_t>(w.size()));
    b.insert(b.end(), w.begin(), w.end());
    return true;
}

bool RdItem(const std::vector<uint8_t>& b, size_t& o, II::Item& out, const char** why) {
    uint32_t n = 0;
    if (!W::RdU32(b, o, n)) return Fail(why, "no entry length");
    if (n == 0) return Fail(why, "empty entry");
    if (n > kMaxItemBytes) return Fail(why, "entry past the byte cap");
    if (b.size() - o < n) return Fail(why, "entry length past the blob");
    const size_t end = o + n;
    if (!DeItem(b, o, out)) return Fail(why, "malformed entry");
    if (o != end) return Fail(why, "entry shorter than its length");
    return true;
}

bool NameOk(const II::Item& it) { return !it.name.empty() && it.name.size() <= kMaxNameChars; }

}  // namespace coop::inventory_item_wire
