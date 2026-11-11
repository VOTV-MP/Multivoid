// coop/player/hand_item_wire.cpp -- see coop/player/hand_item_wire.h.

#include "coop/player/hand_item_wire.h"

#include <cmath>
#include <cstring>

namespace coop::hand_item_wire {
namespace {

void PutStr(std::vector<uint8_t>& out, const std::wstring& s, bool has) {
    const size_t n = has ? (s.size() > kMaxStr ? kMaxStr : s.size()) : 0;
    out.push_back(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(s[i]));
}

}  // namespace

std::vector<uint8_t> Build(const Record& r) {
    std::vector<uint8_t> out;
    out.reserve(kMaxRecordBytes);
    out.push_back(r.slot);
    out.push_back(static_cast<uint8_t>(r.hand));
    out.push_back(r.has ? 1 : 0);
    PutStr(out, r.cls, r.has);
    PutStr(out, r.name, r.has);
    PutStr(out, r.item, r.has);
    if (r.has && r.hand == Hand::Main) {
        const float rel[6] = {r.relPos[0], r.relPos[1], r.relPos[2], r.relRot[0], r.relRot[1], r.relRot[2]};
        const size_t base = out.size();
        out.resize(base + sizeof(rel));
        std::memcpy(out.data() + base, rel, sizeof(rel));
    }
    return out;
}

std::string Parse(const uint8_t* p, size_t len, Record& out) {
    out = Record{};
    if (len < 6) return "payload too short";
    size_t off = 0;
    out.slot = p[off++];
    const uint8_t hand = p[off++];
    if (hand > static_cast<uint8_t>(Hand::Off)) return "a hand out of range";
    out.hand = static_cast<Hand>(hand);
    out.has = p[off++] != 0;
    auto readStr = [&](const char* field, std::wstring& s) -> std::string {
        if (off + 1 > len) return std::string("no ") + field + " field";
        const uint8_t n = p[off++];
        bool control = false;
        for (size_t i = off; i < off + n && i < len; ++i) control = control || p[i] < 0x20 || p[i] == 0x7F;
        const char* why = off + n > len ? "truncated" : n > kMaxStr ? "over the cap" : control ? "with a control character"
                                                                                                 : nullptr;
        if (why) return std::string(field) + " field " + why;
        for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(p[off++]));
        return {};
    };
    struct Field { const char* name; std::wstring* value; };
    const Field fields[] = {{"class", &out.cls}, {"name", &out.name}, {"item", &out.item}};
    for (const Field& f : fields) {
        std::string why = readStr(f.name, *f.value);
        if (!why.empty()) return why;
    }
    if (out.has && out.cls.empty()) return "an item with no class";
    if (!out.has || out.hand != Hand::Main) return {};
    if (off + 6 * sizeof(float) > len) return "missing hold-transform field";
    float rel[6];
    std::memcpy(rel, p + off, sizeof(rel));
    bool sane = true;
    for (int i = 0; i < 6; ++i) sane = sane && std::isfinite(rel[i]);
    for (int i = 0; i < 3; ++i) sane = sane && rel[i] > -300.f && rel[i] < 300.f;
    if (sane) {
        std::memcpy(out.relPos, rel, sizeof(out.relPos));
        std::memcpy(out.relRot, rel + 3, sizeof(out.relRot));
        out.haveRel = true;
    }
    return {};
}

}  // namespace coop::hand_item_wire
