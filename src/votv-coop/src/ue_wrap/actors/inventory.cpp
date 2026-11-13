// ue_wrap/actors/inventory.cpp -- see ue_wrap/actors/inventory.h.
//
// The Fstruct_save record codec + the TArray primitives live in ue_wrap/actors/save_record, which
// coop/props/container_contents_sync needs for the same walk; what stays here is the
// player-scoped concern: the saveSlot resolve, the three player arrays, and Fstruct_equipment.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/actors/prop.h"            // PropFamily
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

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

// ---- live personal store: reflected offsets (resolved once per world; -1 = looked and failed) ----------
// -2 = not looked at yet. Never guessed: an unresolvable offset makes the reader inert, it does
// not fall back to a hardcoded number.
int32_t g_offPlayerContainer  = -2;  // mainGamemode_C.playerContainer  -> Aprop_inventoryContainer_player_C*
int32_t g_offContainerPropInv = -2;  // prop_container_C.propInventory  -> UpropInventory_C*
int32_t g_offInvPlayer        = -2;  // propInventory_C.Player          -> the personal discriminator
int32_t g_offInvIndex         = -2;  // propInventory_C.Index           -> the GObjStack slot
int32_t g_offGObjStack        = -2;  // saveSlot_C.GObjStack

// A miss is remembered only for the world it was made in (a class may not be loaded yet, as at the menu): the next world
// looks again, and says so again. The world is stamped per offset, so one slot's miss in a world does not hide another's.
uint32_t& MissWorldOf(const int32_t& slot) {
    static std::unordered_map<const int32_t*, uint32_t> sMiss;
    return sMiss[&slot];
}

int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -1 && MissWorldOf(slot) != world_identity::Generation()) slot = -2;
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0) {
            MissWorldOf(slot) = world_identity::Generation();
            UE_LOGW("inventory: could not resolve %ls -- what reads it stays off until the next world", name);
        }
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

// An empty equipment slot: no item named and no class in its record. The game finds a free slot by comparing whole
// structs against the struct's default (AddEquipment: Array_Find), so a blank has to be that default, not the zeroed
// transform a blank C++ record holds: a blank written with zeros is never free, and a player whose slots were padded
// that way could wear nothing new. Writing the game's own blank also heals a profile that stored a zeroed one.
bool IsBlankEquip(const EquipRecord& r) {
    return (r.propName.empty() || r.propName == L"None") && r.data.className.empty();
}

// The game's own empty slot, read from the save object being written: element 0 of its `equipment_def` / `hold_def`
// arrays, the defaults saveSlot's reset_player_equipment copies back (`equipment = equipment_def`). Never typed here.
int32_t g_offEquipmentDef = -2;
int32_t g_offHoldDef      = -2;

bool ReadGameBlank(void* save, int32_t& offSlot, const wchar_t* name, EquipRecord& out) {
    const int32_t off = CachedOffset(offSlot, R::ClassOf(save), name);
    if (off < 0) return false;
    const SR::Arr def = SR::ReadArr(save, off);
    if (!def.data || def.num < 1) return false;
    ReadEquipRecord(def.data, out);
    return true;
}

void WriteEquipRecord(uint8_t* base, const EquipRecord& r, const EquipRecord& gameBlank) {
    const EquipRecord& w = IsBlankEquip(r) ? gameBlank : r;
    SR::WriteFNameField(base + kEquip_propName, w.propName);
    SR::WriteFNameField(base + kEquip_propKey, w.propKey);
    // Immediate: this peer's own inventory, written from its own save data in place; no peer names the classes.
    SR::WriteSaveRecord(base + kEquip_data, w.data, SR::ClassLoad::Immediate);  // embedded Fstruct_save (0x100 slot)
    SR::WriteFNameField(base + kEquip_tag, w.tag);
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

std::wstring ReadHeldItemName() {
    void* save = ResolveSaveSlot();
    if (!save) return {};
    const SR::Arr hd = SR::ReadArr(save, kOff_hold);
    if (!hd.data || hd.num < 1) return {};
    std::wstring name = SR::ReadFNameAt(hd.data, kEquip_propName);
    if (name == L"None") name.clear();
    return name;
}

bool WriteHeldItem(const std::wstring& name, const wchar_t* classLeaf, const float* fuel) {
    void* save = ResolveSaveSlot();
    if (!save) return false;
    const SR::Arr hd = SR::ReadArr(save, kOff_hold);
    if (!hd.data || hd.num < 1) return false;
    // Immediate: a drill's own held item, written in place; the class loads if this peer has not loaded it.
    if (classLeaf && !ue_wrap::class_resolve::ResolveClass(classLeaf, ue_wrap::prop::PropFamily(),
                                                          ue_wrap::class_resolve::Load::Immediate).cls)
        return false;
    auto* slot = const_cast<uint8_t*>(hd.data);
    // The record as the game's own pickup leaves it, as far as the hand reads it: the class updateHold
    // spawns, and the name the spawned actor's loadData takes, its first names entry, which
    // propToDynamic also made the slot's own name from; an emptied hand's has neither. The rest of the
    // record stays, and its old buffers are orphaned, as every rewrite of one is.
    SR::SaveRecord rec;
    SR::ReadSaveRecord(slot + kEquip_data, rec);
    rec.className = classLeaf ? classLeaf : L"";
    if (classLeaf) rec.names.assign(1, std::vector<std::wstring>{name});
    else rec.names.clear();
    if (fuel) {
        if (rec.floats.size() < 2) rec.floats.resize(2);
        rec.floats[1].assign(1, *fuel);
    }
    std::memset(slot + kEquip_data, 0, SR::kSaveStride);
    // Immediate: the held item of this peer's own hand, written in place from its own state; nothing to wait for.
    SR::WriteSaveRecord(slot + kEquip_data, rec, SR::ClassLoad::Immediate);
    SR::WriteFNameField(slot + kEquip_propName, name);
    return true;
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
    EquipRecord equipBlank, holdBlank;
    if (!ReadGameBlank(saveSlot, g_offEquipmentDef, L"equipment_def", equipBlank) ||
        !ReadGameBlank(saveSlot, g_offHoldDef, L"hold_def", holdBlank)) {
        UE_LOGE("inventory: ApplyToSaveObject -- save object %p has no equipment_def / hold_def to take the empty slot "
                "from; refusing, since a zeroed blank is a slot the game never sees as free", saveSlot);
        return false;
    }
    // Immediate: the player's own inventory, written from its own save data in place; no peer names the classes.
    BuildAndSwapArray(carried, 0, SR::kSaveStride, inv.inventory,
                      [](uint8_t* e, const SR::SaveRecord& r) { SR::WriteSaveRecord(e, r, SR::ClassLoad::Immediate); });
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
                      [&equipBlank](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r, equipBlank); });
    BuildAndSwapArray(saveSlot, kOff_hold, kEquipStride, slotsOf(kOff_hold, inv.hold),
                      [&holdBlank](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r, holdBlank); });
    UE_LOGI("inventory: ApplyToSaveObject(%p) wrote carried=%zu equipment=%zu hold=%zu",
            saveSlot, inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    return true;
}

bool ZeroEmptyEquipment(void* saveSlot) {
    if (!saveSlot || !R::IsLive(saveSlot)) return false;
    void* probe = R::EngineAlloc(16);  // as the apply: no allocator, no rewrite of arrays that would come back empty
    if (!probe) return false;
    R::EngineFree(probe);
    const EquipRecord zeroed;  // a blank C++ record: the transform zeroed, not the game's identity
    for (const int32_t off : {kOff_equipment, kOff_hold}) {
        const SR::Arr arr = SR::ReadArr(saveSlot, off);
        std::vector<EquipRecord> recs;
        for (int32_t i = 0; i < arr.num; ++i) {
            recs.emplace_back();
            ReadEquipRecord(arr.data + static_cast<size_t>(i) * kEquipStride, recs.back());
        }
        BuildAndSwapArray(saveSlot, off, kEquipStride, recs,
                          [&zeroed](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r, zeroed); });
    }
    return true;
}

}  // namespace ue_wrap::inventory
