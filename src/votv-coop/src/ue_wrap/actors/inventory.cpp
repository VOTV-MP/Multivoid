// ue_wrap/actors/inventory.cpp -- see ue_wrap/actors/inventory.h.
//
// The Fstruct_save record codec + the TArray primitives live in ue_wrap/actors/save_record, which
// coop/props/container_contents_sync needs for the same walk; what stays here is the
// player-scoped concern: the saveSlot resolve, the three player arrays, and Fstruct_equipment.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::inventory {
namespace {

namespace R = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// saveSlot.hpp player-scoped arrays. The carried items are not among them: they live in
// GObjStack[kPersonalSlot], resolved by reflection below.
constexpr int32_t kOff_equipment     = 0x0440;  // TArray<Fstruct_equipment>
constexpr int32_t kOff_hold          = 0x0450;  // TArray<Fstruct_equipment>

// Fstruct_equipment FIELD layout (struct_equipment.hpp; raw size 0x118, ARRAY STRIDE 0x120 -- see
// kEquipStride below).
constexpr int32_t kEquip_propName  = 0x00;   // Fstruct_propDynamic.name FName
constexpr int32_t kEquip_propKey   = 0x08;   // Fstruct_propDynamic.key  FName
constexpr int32_t kEquip_data      = 0x10;   // embedded Fstruct_save (in a 0x100 slot)
constexpr int32_t kEquip_tag       = 0x110;  // FName
// 16-ALIGNED stride (raw `Size: 0x118` -> Align(0x118,16) = 0x120). Fstruct_equipment embeds the
// 16-aligned Fstruct_save, so it too is 16-aligned, and the struct that embeds it reports the
// member as 0x120 wide. Same crash class as save_record::kSaveStride if the raw 0x118 is used for
// the equipment/hold arrays with N>=2.
constexpr int32_t kEquipStride     = 0x120;

// The player's GObjStack slot. Baked, not restored: prop_inventoryContainer_player_C's component
// template serializes index=0, and the saveSlot CDO pre-seeds the slot, so it is the one index
// that is known before any container actor exists -- which is when the join apply runs. The live
// reader checks the running component against it.
constexpr int32_t kPersonalSlot    = 0;

int32_t g_offSave = -1;

// ---- live personal store: reflected offsets (resolved once; -1 = looked and failed) ----------
// -2 = not looked at yet. Never guessed: an unresolvable offset makes the reader inert, it does
// not fall back to a hardcoded number.
int32_t g_offPlayerContainer  = -2;  // mainGamemode_C.playerContainer  -> Aprop_inventoryContainer_player_C*
int32_t g_offContainerPropInv = -2;  // prop_container_C.propInventory  -> UpropInventory_C*
int32_t g_offInvPlayer        = -2;  // propInventory_C.Player          -> the personal discriminator
int32_t g_offInvIndex         = -2;  // propInventory_C.Index           -> the GObjStack slot
int32_t g_offGObjStack        = -2;  // saveSlot_C.GObjStack

int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0)
            UE_LOGW("inventory: could not resolve %ls -- the live-store reader is inert", name);
    }
    return slot;
}

// The contents TArray header of GObjStack[kPersonalSlot] on `save` (the struct_mObject element's
// single field, at +0), or null when the save object has no such slot.
uint8_t* PersonalSlotOf(void* save) {
    if (CachedOffset(g_offGObjStack, R::ClassOf(save), L"GObjStack") < 0) return nullptr;
    const SR::Arr stack = SR::ReadArr(save, g_offGObjStack);
    if (kPersonalSlot >= stack.num) return nullptr;
    return const_cast<uint8_t*>(stack.data) + static_cast<size_t>(kPersonalSlot) * SR::kMxStride;
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

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

// Build one engine TArray<RecordT> from `recs` and overwrite the header at base+off. The
// old buffer is intentionally orphaned (see ApplyToSaveObject's contract).
template <class RecordT, class F>
void BuildAndSwapArray(void* base, int32_t off, int32_t stride,
                       const std::vector<RecordT>& recs, F writeOne) {
    const int32_t num = static_cast<int32_t>(recs.size());
    void* buf = SR::AllocZeroed(static_cast<size_t>(num), static_cast<size_t>(stride));
    if (buf) {
        for (int32_t i = 0; i < num; ++i)
            writeOne(reinterpret_cast<uint8_t*>(buf) + static_cast<size_t>(i) * stride, recs[i]);
        SR::WriteArrHeader(base, off, buf, num);
    } else {
        SR::WriteArrHeader(base, off, nullptr, 0);  // num==0 or alloc failed -> empty
    }
}

// The player's contents array, addressed ONLY through the assertions that prove it is the
// personal store: the container component, its `Player` flag, and its running Index against the
// baked slot. Returns the slot index and fills `contents`, or -1 having read nothing.
//
// Both readers go through it. The count reader used to address GObjStack[0] directly, which is
// the same slot on every build measured -- but "the same slot in practice" is what an address
// assertion exists to stop a reader from relying on, and the count is what the quick-slot edge
// makes its decision on.
int32_t ValidatedPersonalSlot(SR::Arr& contents) {
    void* save = ResolveSaveSlot();
    void* gm = world_singleton::Gamemode();
    if (!save || !gm) return -1;

    if (CachedOffset(g_offPlayerContainer, R::ClassOf(gm), L"playerContainer") < 0) return -1;
    void* pc = ReadAt<void*>(gm, g_offPlayerContainer);
    if (!pc || !SR::PlausibleObjPtr(pc) || !R::IsLive(pc)) return -1;

    if (CachedOffset(g_offContainerPropInv, R::ClassOf(pc), L"propInventory") < 0) return -1;
    void* inv = ReadAt<void*>(pc, g_offContainerPropInv);
    if (!inv || !SR::PlausibleObjPtr(inv) || !R::IsLive(inv)) return -1;

    // ADDRESS ASSERTION (fail-closed): this must be the PERSONAL store and nothing else. Same flag
    // that container_contents_sync's BOUNDARY 1 refuses on -- opposite sides of one boundary.
    if (CachedOffset(g_offInvPlayer, R::ClassOf(inv), L"Player") < 0) return -1;
    if (ReadAt<uint8_t>(inv, g_offInvPlayer) == 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("inventory: playerContainer.propInventory is NOT flagged personal (Player==0) "
                    "-- refusing to read it as the live personal store. Expected player=True from "
                    "the class's component template; something resolved to the wrong container.");
        }
        return -1;
    }

    if (CachedOffset(g_offInvIndex, R::ClassOf(inv), L"Index") < 0) return -1;
    const int32_t idx = ReadAt<int32_t>(inv, g_offInvIndex);
    if (idx < 0) return -1;  // -1 = never initialised
    if (idx != kPersonalSlot) {
        // The join apply wrote kPersonalSlot before this component existed. A different running
        // index means the apply and the game disagree about where the player's items live.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGE("inventory: the player container runs on GObjStack[%d], not the baked slot %d "
                    "-- the join apply addressed the wrong slot", idx, kPersonalSlot);
        }
    }

    if (CachedOffset(g_offGObjStack, R::ClassOf(save), L"GObjStack") < 0) return -1;
    const SR::Arr stack = SR::ReadArr(save, g_offGObjStack);
    if (idx >= stack.num) return -1;

    // The struct_mObject element's single field is the contents TArray<Fstruct_save> at +0.
    contents = SR::ReadArr(stack.data + static_cast<size_t>(idx) * SR::kMxStride, 0);
    return idx;
}

}  // namespace

void* ResolveSaveSlot() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

bool ReadAll(PlayerInventory& out) {
    out.inventory.clear();
    out.equipment.clear();
    out.hold.clear();
    LivePersonalStore live;
    if (!ReadLivePersonalStore(live)) return false;  // pre-world: no container, nothing carried yet
    void* save = ResolveSaveSlot();
    if (!save) return false;
    out.inventory = std::move(live.records);
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

int32_t LivePersonalStoreCount() {
    SR::Arr contents{};
    return ValidatedPersonalSlot(contents) < 0 ? -1 : contents.num;
}

bool ReadLivePersonalStore(LivePersonalStore& out) {
    out.slotIndex = -1;
    out.records.clear();
    SR::Arr contents{};
    const int32_t idx = ValidatedPersonalSlot(contents);
    if (idx < 0) return false;

    out.slotIndex = idx;
    out.records.reserve(static_cast<size_t>(contents.num));
    for (int32_t i = 0; i < contents.num; ++i) {
        SR::SaveRecord r;
        SR::ReadSaveRecord(contents.data + static_cast<size_t>(i) * SR::kSaveStride, r);
        out.records.push_back(std::move(r));
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
    uint8_t* carried = PersonalSlotOf(saveSlot);
    if (!carried) {
        UE_LOGE("inventory: ApplyToSaveObject -- save object %p has no GObjStack[%d]; refusing to "
                "write a partial inventory", saveSlot, kPersonalSlot);
        return false;
    }
    BuildAndSwapArray(carried, 0, SR::kSaveStride, inv.inventory,
                      [](uint8_t* e, const SR::SaveRecord& r) { SR::WriteSaveRecord(e, r); });
    // equipment and hold are FIXED-SHAPE slot arrays: the game finds an empty slot to equip into
    // (AddEquipment: Array_Find, "Not enough space" on -1) and writes hold[0] in place, and
    // nothing in it ever grows them. A profile with fewer slots than the save object -- an empty
    // one above all -- is padded with blank slots to the shape the save object came with.
    auto slotsOf = [saveSlot](int32_t off, const std::vector<EquipRecord>& recs) {
        std::vector<EquipRecord> out = recs;
        const size_t shape = static_cast<size_t>(SR::ReadArr(saveSlot, off).num);
        if (out.size() < shape) out.resize(shape);
        return out;
    };
    BuildAndSwapArray(saveSlot, kOff_equipment, kEquipStride, slotsOf(kOff_equipment, inv.equipment),
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    BuildAndSwapArray(saveSlot, kOff_hold, kEquipStride, slotsOf(kOff_hold, inv.hold),
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    UE_LOGI("inventory: ApplyToSaveObject(%p) wrote carried=%zu equipment=%zu hold=%zu",
            saveSlot, inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    return true;
}

}  // namespace ue_wrap::inventory
