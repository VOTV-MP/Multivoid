// coop/inventory_wire.cpp -- see coop/inventory_wire.h.

#include "coop/inventory_wire.h"

#include "coop/signal_wire.h"  // reuse the proven 0x70 signal-row serializer
#include "ue_wrap/log.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace coop::inventory_wire {
namespace {

using ue_wrap::inventory::SaveRecord;
using ue_wrap::inventory::EquipRecord;
using ue_wrap::inventory::PlayerInventory;

// Sanity caps (a corrupt / hostile blob must never drive a huge allocation or over-read). These
// are generous-but-realistic absolute ceilings; the PRIMARY bound is the per-count feasibility
// check Feasible() below -- each declared element needs >= 1 wire byte, so a count can never exceed
// the bytes remaining. That makes every reserve/resize proportional to the ACTUAL blob size (a
// small tampered-but-FNV-consistent file can no longer declare a count that balloons RSS to tens
// of MB before the parse fails). (Adversarial-verify MEDIUM, 2026-06-14.)
constexpr uint32_t kMaxRecords = 16384;
constexpr uint32_t kMaxGroups  = 4096;
constexpr uint32_t kMaxElems   = 262144;
constexpr uint32_t kMaxStrLen  = 262144;

// A declared count of `n` elements is feasible only if at least `n` bytes remain (1-byte floor per
// element). Bounds every count-driven reserve/resize to the real blob size.
bool Feasible(uint32_t n, const std::vector<uint8_t>& b, size_t o) {
    return n <= b.size() - o;  // o <= b.size() always (every prior read is bounds-checked)
}

// ---- append (little-endian) ----
void AppU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
template <class T> void AppPod(std::vector<uint8_t>& b, T v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
void AppU32(std::vector<uint8_t>& b, uint32_t v) { AppPod(b, v); }
void AppI32(std::vector<uint8_t>& b, int32_t v) { AppPod(b, v); }
void AppF32(std::vector<uint8_t>& b, float v) { AppPod(b, v); }
void AppWStr(std::vector<uint8_t>& b, const std::wstring& s) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    AppU32(b, n);
    for (uint32_t i = 0; i < n; ++i) {                 // UTF-16LE; preserves exact code units (case)
        const uint16_t c = static_cast<uint16_t>(s[i]);
        b.push_back(static_cast<uint8_t>(c & 0xFF));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
}

// ---- read (cursor + bounds; every Read fails cleanly on overrun) ----
bool RdU8(const std::vector<uint8_t>& b, size_t& o, uint8_t& v) {
    if (o + 1 > b.size()) return false; v = b[o++]; return true;
}
template <class T> bool RdPod(const std::vector<uint8_t>& b, size_t& o, T& v) {
    if (o + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T); return true;
}
bool RdU32(const std::vector<uint8_t>& b, size_t& o, uint32_t& v) { return RdPod(b, o, v); }
bool RdI32(const std::vector<uint8_t>& b, size_t& o, int32_t& v) { return RdPod(b, o, v); }
bool RdF32(const std::vector<uint8_t>& b, size_t& o, float& v) { return RdPod(b, o, v); }
bool RdWStr(const std::vector<uint8_t>& b, size_t& o, std::wstring& s) {
    uint32_t n;
    if (!RdU32(b, o, n) || n > kMaxStrLen || o + static_cast<size_t>(n) * 2 > b.size()) return false;
    s.clear(); s.reserve(n);
    for (uint32_t i = 0; i < n; ++i) { s.push_back(static_cast<wchar_t>(b[o] | (b[o + 1] << 8))); o += 2; }
    return true;
}

// ---- generic value-group (vector<vector<T>>) ----
template <class T, class AppOne>
void SerGroups(std::vector<uint8_t>& b, const std::vector<std::vector<T>>& gs, AppOne appOne) {
    AppU32(b, static_cast<uint32_t>(gs.size()));
    for (const auto& g : gs) {
        AppU32(b, static_cast<uint32_t>(g.size()));
        for (const auto& e : g) appOne(b, e);
    }
}
template <class T, class RdOne>
bool DeGroups(const std::vector<uint8_t>& b, size_t& o, std::vector<std::vector<T>>& gs, RdOne rdOne) {
    uint32_t gc; if (!RdU32(b, o, gc) || gc > kMaxGroups || !Feasible(gc, b, o)) return false;
    gs.clear(); gs.resize(gc);
    for (auto& g : gs) {
        uint32_t ic; if (!RdU32(b, o, ic) || ic > kMaxElems || !Feasible(ic, b, o)) return false;
        g.resize(ic);
        for (auto& e : g) if (!rdOne(b, o, e)) return false;
    }
    return true;
}

void SerSave(std::vector<uint8_t>& b, const SaveRecord& r) {
    AppWStr(b, r.className);
    for (float f : r.xform) AppF32(b, f);
    AppWStr(b, r.key);
    SerGroups(b, r.bools,      [](std::vector<uint8_t>& bb, uint8_t e) { AppU8(bb, e); });
    SerGroups(b, r.floats,     [](std::vector<uint8_t>& bb, float e) { AppF32(bb, e); });
    SerGroups(b, r.ints,       [](std::vector<uint8_t>& bb, int32_t e) { AppI32(bb, e); });
    SerGroups(b, r.strings,    [](std::vector<uint8_t>& bb, const std::wstring& e) { AppWStr(bb, e); });
    SerGroups(b, r.classes,    [](std::vector<uint8_t>& bb, const std::wstring& e) { AppWStr(bb, e); });
    SerGroups(b, r.vectors,    [](std::vector<uint8_t>& bb, const std::array<float, 3>& e) { for (float f : e) AppF32(bb, f); });
    SerGroups(b, r.rotators,   [](std::vector<uint8_t>& bb, const std::array<float, 3>& e) { for (float f : e) AppF32(bb, f); });
    SerGroups(b, r.transforms, [](std::vector<uint8_t>& bb, const std::array<float, 10>& e) { for (float f : e) AppF32(bb, f); });
    SerGroups(b, r.bytes,      [](std::vector<uint8_t>& bb, uint8_t e) { AppU8(bb, e); });
    SerGroups(b, r.names,      [](std::vector<uint8_t>& bb, const std::wstring& e) { AppWStr(bb, e); });
    // signals: reuse signal_wire::Serialize per row, each length-prefixed.
    AppU32(b, static_cast<uint32_t>(r.signals.size()));
    for (const auto& row : r.signals) {
        const std::vector<uint8_t> sub = coop::signal_wire::Serialize(row, /*adopt=*/false);
        AppU32(b, static_cast<uint32_t>(sub.size()));
        b.insert(b.end(), sub.begin(), sub.end());
    }
}

bool DeSave(const std::vector<uint8_t>& b, size_t& o, SaveRecord& r) {
    if (!RdWStr(b, o, r.className)) return false;
    for (float& f : r.xform) if (!RdF32(b, o, f)) return false;
    if (!RdWStr(b, o, r.key)) return false;
    if (!DeGroups(b, o, r.bools,   [](const std::vector<uint8_t>& bb, size_t& oo, uint8_t& e) { return RdU8(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.floats,  [](const std::vector<uint8_t>& bb, size_t& oo, float& e) { return RdF32(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.ints,    [](const std::vector<uint8_t>& bb, size_t& oo, int32_t& e) { return RdI32(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.strings, [](const std::vector<uint8_t>& bb, size_t& oo, std::wstring& e) { return RdWStr(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.classes, [](const std::vector<uint8_t>& bb, size_t& oo, std::wstring& e) { return RdWStr(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.vectors, [](const std::vector<uint8_t>& bb, size_t& oo, std::array<float, 3>& e) { for (float& f : e) if (!RdF32(bb, oo, f)) return false; return true; })) return false;
    if (!DeGroups(b, o, r.rotators,[](const std::vector<uint8_t>& bb, size_t& oo, std::array<float, 3>& e) { for (float& f : e) if (!RdF32(bb, oo, f)) return false; return true; })) return false;
    if (!DeGroups(b, o, r.transforms,[](const std::vector<uint8_t>& bb, size_t& oo, std::array<float, 10>& e) { for (float& f : e) if (!RdF32(bb, oo, f)) return false; return true; })) return false;
    if (!DeGroups(b, o, r.bytes,   [](const std::vector<uint8_t>& bb, size_t& oo, uint8_t& e) { return RdU8(bb, oo, e); })) return false;
    if (!DeGroups(b, o, r.names,   [](const std::vector<uint8_t>& bb, size_t& oo, std::wstring& e) { return RdWStr(bb, oo, e); })) return false;
    uint32_t sc; if (!RdU32(b, o, sc) || sc > kMaxElems || !Feasible(sc, b, o)) return false;
    r.signals.clear(); r.signals.reserve(sc);
    for (uint32_t i = 0; i < sc; ++i) {
        uint32_t subLen; if (!RdU32(b, o, subLen) || o + subLen > b.size()) return false;
        std::vector<uint8_t> sub(b.begin() + static_cast<std::ptrdiff_t>(o),
                                 b.begin() + static_cast<std::ptrdiff_t>(o + subLen));
        o += subLen;
        ue_wrap::signal_dynamic::Row row; bool adopt = false;
        if (!coop::signal_wire::Deserialize(sub, row, adopt)) return false;
        r.signals.push_back(std::move(row));
    }
    return true;
}

void SerEquip(std::vector<uint8_t>& b, const EquipRecord& r) {
    AppWStr(b, r.propName);
    AppWStr(b, r.propKey);
    SerSave(b, r.data);
    AppWStr(b, r.tag);
}
bool DeEquip(const std::vector<uint8_t>& b, size_t& o, EquipRecord& r) {
    if (!RdWStr(b, o, r.propName)) return false;
    if (!RdWStr(b, o, r.propKey)) return false;
    if (!DeSave(b, o, r.data)) return false;
    return RdWStr(b, o, r.tag);
}

}  // namespace

std::vector<uint8_t> Serialize(const PlayerInventory& inv) {
    std::vector<uint8_t> b;
    b.push_back(kVersion);
    AppU32(b, static_cast<uint32_t>(inv.inventory.size()));
    for (const auto& r : inv.inventory) SerSave(b, r);
    AppU32(b, static_cast<uint32_t>(inv.equipment.size()));
    for (const auto& r : inv.equipment) SerEquip(b, r);
    AppU32(b, static_cast<uint32_t>(inv.hold.size()));
    for (const auto& r : inv.hold) SerEquip(b, r);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, PlayerInventory& out) {
    out.inventory.clear(); out.equipment.clear(); out.hold.clear();
    size_t o = 0;
    uint8_t ver; if (!RdU8(b, o, ver) || ver != kVersion) return false;
    uint32_t n;
    if (!RdU32(b, o, n) || n > kMaxRecords || !Feasible(n, b, o)) return false;
    out.inventory.resize(n);
    for (auto& r : out.inventory) if (!DeSave(b, o, r)) return false;
    if (!RdU32(b, o, n) || n > kMaxRecords || !Feasible(n, b, o)) return false;
    out.equipment.resize(n);
    for (auto& r : out.equipment) if (!DeEquip(b, o, r)) return false;
    if (!RdU32(b, o, n) || n > kMaxRecords || !Feasible(n, b, o)) return false;
    out.hold.resize(n);
    for (auto& r : out.hold) if (!DeEquip(b, o, r)) return false;
    return true;
}

}  // namespace coop::inventory_wire
