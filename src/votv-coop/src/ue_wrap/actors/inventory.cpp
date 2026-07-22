// ue_wrap/inventory.cpp -- see ue_wrap/inventory.h.
//
// The Fstruct_save record codec + the TArray primitives moved to ue_wrap/actors/save_record
// (2026-07-22) when coop/props/container_contents_sync needed the same walk; what stays here is
// the player-scoped concern: the saveSlot resolve, the three player arrays, and Fstruct_equipment.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::inventory {
namespace {

namespace R = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// saveSlot.hpp player-scoped arrays.
constexpr int32_t kOff_inventoryData = 0x02E0;  // TArray<Fstruct_save>
constexpr int32_t kOff_equipment     = 0x0440;  // TArray<Fstruct_equipment>
constexpr int32_t kOff_hold          = 0x0450;  // TArray<Fstruct_equipment>

// Fstruct_equipment FIELD layout (struct_equipment.hpp; raw size 0x118, ARRAY STRIDE 0x120 -- see
// kEquipStride below).
constexpr int32_t kEquip_propName  = 0x00;   // Fstruct_propDynamic.name FName
constexpr int32_t kEquip_propKey   = 0x08;   // Fstruct_propDynamic.key  FName
constexpr int32_t kEquip_data      = 0x10;   // embedded Fstruct_save (in a 0x100 slot)
constexpr int32_t kEquip_tag       = 0x110;  // FName
// 16-ALIGNED stride (raw `Size: 0x118` -> Align(0x118,16) = 0x120). Fstruct_equipment embeds the
// 16-aligned Fstruct_save, so it too is 16-aligned. SDK-confirmed: struct_equipmentWear.hpp:6
// reports the embedded Fstruct_equipment as size 0x120 (next field @0x120). Same crash class as
// save_record::kSaveStride if the raw 0x118 is used for the equipment/hold arrays with N>=2.
constexpr int32_t kEquipStride     = 0x120;

void*   g_gm      = nullptr;
int32_t g_offSave = -1;

void ReadEquipRecord(const void* base, EquipRecord& rec) {
    rec.propName = SR::ReadFNameAt(base, kEquip_propName);
    rec.propKey  = SR::ReadFNameAt(base, kEquip_propKey);
    SR::ReadSaveRecord(reinterpret_cast<const uint8_t*>(base) + kEquip_data, rec.data);
    rec.tag      = SR::ReadFNameAt(base, kEquip_tag);
}

void WriteEquipRecord(uint8_t* base, const EquipRecord& r) {
    SR::WriteFNameField(base + kEquip_propName, r.propName);
    SR::WriteFNameField(base + kEquip_propKey, r.propKey);
    SR::WriteSaveRecord(base + kEquip_data, r.data);  // embedded Fstruct_save (0x100 slot)
    SR::WriteFNameField(base + kEquip_tag, r.tag);
}

// Build one engine TArray<RecordT> from `recs` and overwrite the header at saveSlot+off. The
// old buffer is intentionally orphaned (see ApplyToSaveObject's contract).
template <class RecordT, class F>
void BuildAndSwapArray(void* saveSlot, int32_t off, int32_t stride,
                       const std::vector<RecordT>& recs, F writeOne) {
    const int32_t num = static_cast<int32_t>(recs.size());
    void* buf = SR::AllocZeroed(static_cast<size_t>(num), static_cast<size_t>(stride));
    if (buf) {
        for (int32_t i = 0; i < num; ++i)
            writeOne(reinterpret_cast<uint8_t*>(buf) + static_cast<size_t>(i) * stride, recs[i]);
        SR::WriteArrHeader(saveSlot, off, buf, num);
    } else {
        SR::WriteArrHeader(saveSlot, off, nullptr, 0);  // num==0 or alloc failed -> empty
    }
}

}  // namespace

void* ResolveSaveSlot() {
    if (!g_gm || !R::IsLive(g_gm)) g_gm = R::FindObjectByClass(L"mainGamemode_C");
    if (!g_gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(g_gm), L"saveSlot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

bool ReadAll(PlayerInventory& out) {
    out.inventory.clear();
    out.equipment.clear();
    out.hold.clear();
    void* save = ResolveSaveSlot();
    if (!save) return false;
    const SR::Arr inv = SR::ReadArr(save, kOff_inventoryData);
    out.inventory.reserve(static_cast<size_t>(inv.num));
    for (int32_t i = 0; i < inv.num; ++i) {
        SR::SaveRecord r;
        SR::ReadSaveRecord(inv.data + static_cast<size_t>(i) * SR::kSaveStride, r);
        out.inventory.push_back(std::move(r));
    }
    const SR::Arr eq = SR::ReadArr(save, kOff_equipment);
    out.equipment.reserve(static_cast<size_t>(eq.num));
    for (int32_t i = 0; i < eq.num; ++i) {
        EquipRecord r;
        ReadEquipRecord(eq.data + static_cast<size_t>(i) * kEquipStride, r);
        out.equipment.push_back(std::move(r));
    }
    const SR::Arr hd = SR::ReadArr(save, kOff_hold);
    out.hold.reserve(static_cast<size_t>(hd.num));
    for (int32_t i = 0; i < hd.num; ++i) {
        EquipRecord r;
        ReadEquipRecord(hd.data + static_cast<size_t>(i) * kEquipStride, r);
        out.hold.push_back(std::move(r));
    }
    return true;
}

bool ApplyToSaveObject(void* saveSlot, const PlayerInventory& inv) {
    if (!saveSlot || !R::IsLive(saveSlot)) return false;
    if (void* probe = R::EngineAlloc(16)) {  // GMalloc probe: a failure -> we'd silently write
        R::EngineFree(probe);                // empty arrays over a real inventory; refuse instead
    } else {
        UE_LOGW("inventory: ApplyToSaveObject -- EngineAlloc unavailable (GMalloc unresolved); "
                "refusing to write empty inventory over saveSlot %p", saveSlot);
        return false;
    }
    BuildAndSwapArray(saveSlot, kOff_inventoryData, SR::kSaveStride, inv.inventory,
                      [](uint8_t* e, const SR::SaveRecord& r) { SR::WriteSaveRecord(e, r); });
    BuildAndSwapArray(saveSlot, kOff_equipment, kEquipStride, inv.equipment,
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    BuildAndSwapArray(saveSlot, kOff_hold, kEquipStride, inv.hold,
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    UE_LOGI("inventory: ApplyToSaveObject(%p) wrote inventory=%zu equipment=%zu hold=%zu",
            saveSlot, inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    return true;
}

}  // namespace ue_wrap::inventory
