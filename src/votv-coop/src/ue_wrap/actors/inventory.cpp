// ue_wrap/actors/inventory.cpp -- see ue_wrap/actors/inventory.h.
//
// The entry codec is ue_wrap/actors/inventory_item's, shared with the containers' inventories; what stays here is
// the player-scoped concern: the pawn's inventory component, its two arrays, and the hands' item names.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/actors/player_record.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace ue_wrap::inventory {
namespace {

namespace R = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace II = ue_wrap::inventory_item;

int32_t g_offSave = -1;

R::InstanceOffset g_mainPlayer(L"mainPlayer");                    // on the gamemode
R::InstanceOffset g_component(L"inventoryComponent_player");      // on the pawn
R::InstanceOffset g_slotsA(L"slotArray_A");                       // on the component: the hotbar slots
R::InstanceOffset g_canvas(L"canvasInventory");                   // ... the bag (what addObjectToInventory fills)
R::InstanceOffset g_slotsB(L"slotArray_B");                       // ... worn
R::InstanceOffset g_heldMain(L"holdingObjectName_main");          // on the pawn
R::InstanceOffset g_heldOff(L"holdingObjectName_offhand");

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// The local player's pawn: the gamemode's own `mainPlayer`, live. Null when there is none.
void* LocalPawn() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    const int32_t off = g_mainPlayer.Of(gm);
    if (off < 0) return nullptr;
    void* const pawn = ReadAt<void*>(gm, off);
    return pawn && SR::PlausibleObjPtr(pawn) && R::IsLive(pawn) ? pawn : nullptr;
}

// The pawn's inventory component, live; null when it does not resolve.
void* Component() {
    void* const pawn = LocalPawn();
    if (!pawn) return nullptr;
    const int32_t off = g_component.Of(pawn);
    if (off < 0) return nullptr;
    void* const comp = ReadAt<void*>(pawn, off);
    return comp && SR::PlausibleObjPtr(comp) && R::IsLive(comp) ? comp : nullptr;
}

// The component's array `slots` with the entry layout resolved: a null data pointer when either does not.
SR::Arr SlotsOf(void* comp, R::InstanceOffset& slots) {
    if (!comp || !II::Resolve()) return SR::Arr{};
    const int32_t off = slots.Of(comp);
    return off < 0 ? SR::Arr{} : SR::ReadArr(comp, off);
}

// An entry as the SaveRecord the readers hand out (see PlayerInventory).
SR::SaveRecord AsRecord(const II::Item& it) {
    SR::SaveRecord r;
    r.className = II::ClassLeaf(it);
    r.xform = it.xform;
    r.key = it.key;
    r.names.push_back({it.name, it.tag});
    return r;
}

// The non-empty entries of the component's array `slots`, appended to `out` (not cleared). False when the component, the
// array or the layout does not resolve.
bool AppendSlots(void* comp, R::InstanceOffset& slots, std::vector<SR::SaveRecord>& out) {
    if (!comp || !II::Resolve() || slots.Of(comp) < 0) return false;
    const SR::Arr a = SlotsOf(comp, slots);
    const int32_t stride = II::Stride();
    for (int32_t i = 0; i < a.num; ++i) {
        II::Item it;
        if (!II::Read(a.data + static_cast<size_t>(i) * stride, it) || II::IsEmpty(it)) continue;
        out.push_back(AsRecord(it));
    }
    return true;
}

// The non-empty entries of the component's array `slots` into `out`.
bool ReadSlots(void* comp, R::InstanceOffset& slots, std::vector<SR::SaveRecord>& out) {
    out.clear();
    return AppendSlots(comp, slots, out);
}

// What the player carries: the hotbar (slotArray_A), then the bag (canvasInventory).
bool ReadCarried(void* comp, std::vector<SR::SaveRecord>& out) {
    out.clear();
    return AppendSlots(comp, g_slotsA, out) && AppendSlots(comp, g_canvas, out);
}

}  // namespace

void* ResolveSaveSlot() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(gm), L"save_slot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

bool ReadAll(PlayerInventory& out) {
    out.equipment.clear();
    void* const comp = Component();
    return ReadCarried(comp, out.inventory) && ReadSlots(comp, g_slotsB, out.equipment);
}

bool ReadLivePersonalStore(LivePersonalStore& out) {
    return ReadCarried(Component(), out.records);
}

namespace {
// The non-empty entries of `slots`, whole, into `out` (cleared first). False when the layout or the array does not resolve.
bool ReadItems(void* comp, R::InstanceOffset& slots, std::vector<II::Item>& out) {
    out.clear();
    if (!comp || !II::Resolve() || slots.Of(comp) < 0) return false;
    const SR::Arr a = SlotsOf(comp, slots);
    const int32_t stride = II::Stride();
    for (int32_t i = 0; i < a.num; ++i) {
        II::Item it;
        if (II::Read(a.data + static_cast<size_t>(i) * stride, it) && !II::IsEmpty(it)) out.push_back(std::move(it));
    }
    return true;
}
}  // namespace

bool ReadLiveItems(std::vector<II::Item>* hotbar, std::vector<II::Item>* bag) {
    void* const comp = Component();
    std::vector<II::Item> a, b;
    if (!ReadItems(comp, g_slotsA, a) || !ReadItems(comp, g_canvas, b)) return false;
    if (hotbar) *hotbar = std::move(a);
    if (bag) *bag = std::move(b);
    return true;
}

int32_t LivePersonalStoreCount() {
    std::vector<SR::SaveRecord> carried;
    return ReadCarried(Component(), carried) ? static_cast<int32_t>(carried.size()) : -1;
}

std::wstring ReadHeldItemName(Hand hand) {
    void* const pawn = LocalPawn();
    if (!pawn) return {};
    const int32_t off = (hand == Hand::Main ? g_heldMain : g_heldOff).Of(pawn);
    if (off < 0) return {};
    std::wstring name = SR::ReadFNameAt(pawn, off);
    if (name == L"None") name.clear();
    return name;
}

bool WriteWornItemProperty(const wchar_t* classLeaf, const wchar_t* prop, const std::wstring& value) {
    void* const comp = Component();
    if (!classLeaf || !prop || !comp || !II::Resolve() || g_slotsB.Of(comp) < 0) return false;
    void* const cls = R::FindClass(classLeaf);
    if (!cls) return false;
    const SR::Arr a = SlotsOf(comp, g_slotsB);
    const int32_t stride = II::Stride();
    for (int32_t i = 0; i < a.num; ++i) {
        uint8_t* const e = const_cast<uint8_t*>(a.data) + static_cast<size_t>(i) * stride;
        II::Item it;
        if (!II::Read(e, it) || II::IsEmpty(it) || ::_wcsicmp(II::ClassLeaf(it).c_str(), classLeaf) != 0) continue;
        if (!ue_wrap::player_record::SetPropertyInBytes(cls, it.actorData, prop, value)) return false;
        // The entry is rewritten whole; the buffers it held are orphaned, as every rewrite of one is.
        std::memset(e, 0, static_cast<size_t>(stride));
        return II::Write(e, it);
    }
    return false;
}

bool ReadWornItemProperty(const wchar_t* classLeaf, const wchar_t* prop, std::wstring& value) {
    value.clear();
    void* const comp = Component();
    if (!classLeaf || !prop || !comp || !II::Resolve() || g_slotsB.Of(comp) < 0) return false;
    void* const cls = R::FindClass(classLeaf);
    if (!cls) return false;
    const SR::Arr a = SlotsOf(comp, g_slotsB);
    const int32_t stride = II::Stride();
    for (int32_t i = 0; i < a.num; ++i) {
        const uint8_t* const e = a.data + static_cast<size_t>(i) * stride;
        II::Item it;
        if (!II::Read(e, it) || II::IsEmpty(it) || ::_wcsicmp(II::ClassLeaf(it).c_str(), classLeaf) != 0) continue;
        return ue_wrap::player_record::GetPropertyInBytes(cls, it.actorData, prop, value);
    }
    return false;
}

}  // namespace ue_wrap::inventory
