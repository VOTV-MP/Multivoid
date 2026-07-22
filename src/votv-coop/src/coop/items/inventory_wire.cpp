// coop/inventory_wire.cpp -- see coop/inventory_wire.h.
//
// The per-record grammar + the byte primitives moved to coop/items/save_record_wire (2026-07-22)
// when coop/props/container_contents_sync needed the same codec; what stays here is the
// player-scoped envelope: the version byte, the three arrays, and Fstruct_equipment.

#include "coop/items/inventory_wire.h"

#include "coop/items/save_record_wire.h"

#include <cstddef>

namespace coop::inventory_wire {
namespace {

namespace W = coop::save_record_wire;

using ue_wrap::inventory::EquipRecord;
using ue_wrap::inventory::PlayerInventory;

void SerEquip(std::vector<uint8_t>& b, const EquipRecord& r) {
    W::AppWStr(b, r.propName);
    W::AppWStr(b, r.propKey);
    W::SerSave(b, r.data);
    W::AppWStr(b, r.tag);
}
bool DeEquip(const std::vector<uint8_t>& b, size_t& o, EquipRecord& r) {
    if (!W::RdWStr(b, o, r.propName)) return false;
    if (!W::RdWStr(b, o, r.propKey)) return false;
    if (!W::DeSave(b, o, r.data)) return false;
    return W::RdWStr(b, o, r.tag);
}

}  // namespace

std::vector<uint8_t> Serialize(const PlayerInventory& inv) {
    std::vector<uint8_t> b;
    b.push_back(kVersion);
    W::AppU32(b, static_cast<uint32_t>(inv.inventory.size()));
    for (const auto& r : inv.inventory) W::SerSave(b, r);
    W::AppU32(b, static_cast<uint32_t>(inv.equipment.size()));
    for (const auto& r : inv.equipment) SerEquip(b, r);
    W::AppU32(b, static_cast<uint32_t>(inv.hold.size()));
    for (const auto& r : inv.hold) SerEquip(b, r);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, PlayerInventory& out) {
    out.inventory.clear(); out.equipment.clear(); out.hold.clear();
    size_t o = 0;
    uint8_t ver; if (!W::RdU8(b, o, ver) || ver != kVersion) return false;
    uint32_t n;
    if (!W::RdU32(b, o, n) || n > W::kMaxRecords || !W::Feasible(n, b, o)) return false;
    out.inventory.resize(n);
    for (auto& r : out.inventory) if (!W::DeSave(b, o, r)) return false;
    if (!W::RdU32(b, o, n) || n > W::kMaxRecords || !W::Feasible(n, b, o)) return false;
    out.equipment.resize(n);
    for (auto& r : out.equipment) if (!DeEquip(b, o, r)) return false;
    if (!W::RdU32(b, o, n) || n > W::kMaxRecords || !W::Feasible(n, b, o)) return false;
    out.hold.resize(n);
    for (auto& r : out.hold) if (!DeEquip(b, o, r)) return false;
    return true;
}

}  // namespace coop::inventory_wire
