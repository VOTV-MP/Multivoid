// ue_wrap/actors/container_inventory.cpp -- see ue_wrap/actors/container_inventory.h.

#include "ue_wrap/actors/container_inventory.h"

#include "ue_wrap/actors/inventory.h"      // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"           // WalksToBase
#include "ue_wrap/core/component_calls.h"  // CallParamless
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstring>

namespace ue_wrap::container_inventory {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

int32_t g_offInvIndex  = -2;  // propInventory_C.Index
int32_t g_offInvPlayer = -2;  // propInventory_C.Player  -- the world-vs-PERSONAL discriminator
int32_t g_offInvOwner  = -2;  // propInventory_C.Owner   -- the Aprop_container_C
int32_t g_offGObjStack = -2;  // saveSlot_C.GObjStack
int32_t g_offPropInv   = -2;  // prop_container_C.propInventory
int32_t g_offCurrVol   = -2;  // propInventory_C.currVol

int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0)
            UE_LOGW("container_inventory: could not resolve %ls -- the contents lane is inert for it", name);
    }
    return slot;
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// Looked up per use, one index lookup each, so a class not loaded yet is asked for again.
void* ContainerClass() { return ue_wrap::object_index::ClassByName(L"prop_container_C"); }
void* InventoryClass() { return ue_wrap::object_index::ClassByName(L"propInventory_C"); }

}  // namespace

bool IsContainer(void* actor) {
    void* base = ContainerClass();
    return base && actor && ue_wrap::prop::WalksToBase(R::ClassOf(actor), base);
}

bool IsInventory(void* obj) {
    void* base = InventoryClass();
    return base && obj && ue_wrap::prop::WalksToBase(R::ClassOf(obj), base);
}

void* InventoryOf(void* container) {
    if (!container) return nullptr;
    if (CachedOffset(g_offPropInv, R::ClassOf(container), L"propInventory") < 0) return nullptr;
    void* inv = ReadAt<void*>(container, g_offPropInv);
    return (inv && R::IsLive(inv)) ? inv : nullptr;
}

void* OwnerOf(void* inventory) {
    if (!inventory) return nullptr;
    if (CachedOffset(g_offInvOwner, R::ClassOf(inventory), L"Owner") < 0) return nullptr;
    void* owner = ReadAt<void*>(inventory, g_offInvOwner);
    return (owner && R::IsLive(owner)) ? owner : nullptr;
}

bool IsWorldInventory(void* inventory) {
    if (!inventory) return false;
    if (CachedOffset(g_offInvPlayer, R::ClassOf(inventory), L"Player") < 0) return false;
    return ReadAt<uint8_t>(inventory, g_offInvPlayer) == 0;
}

uint8_t* ContentsSlot(void* inventory) {
    if (!inventory) return nullptr;
    void* save = ue_wrap::inventory::ResolveSaveSlot();
    if (!save) return nullptr;
    if (CachedOffset(g_offGObjStack, R::ClassOf(save), L"GObjStack") < 0) return nullptr;
    if (CachedOffset(g_offInvIndex, R::ClassOf(inventory), L"Index") < 0) return nullptr;
    const int32_t idx = ReadAt<int32_t>(inventory, g_offInvIndex);
    if (idx < 0) return nullptr;
    const SR::Arr stack = SR::ReadArr(save, g_offGObjStack);
    if (idx >= stack.num) return nullptr;
    return const_cast<uint8_t*>(stack.data) + static_cast<size_t>(idx) * SR::kMxStride;
}

bool ReadContents(void* inventory, std::vector<SR::SaveRecord>& out, size_t maxRecords,
                  int32_t* countOut) {
    if (countOut) *countOut = -1;
    uint8_t* slot = ContentsSlot(inventory);
    if (!slot) return false;
    const SR::Arr objs = SR::ReadArr(slot, 0);  // struct_mObject.obj @ +0
    if (countOut) *countOut = objs.num;
    if (static_cast<size_t>(objs.num) > maxRecords) return false;
    out.clear();
    out.reserve(static_cast<size_t>(objs.num));
    for (int32_t i = 0; i < objs.num; ++i) {
        SR::SaveRecord r;
        SR::ReadSaveRecord(objs.data + static_cast<size_t>(i) * SR::kSaveStride, r);
        out.push_back(std::move(r));
    }
    return true;
}

bool IsContainerRecord(const SR::SaveRecord& r) {
    if (r.className.empty()) return false;
    void* const base = ContainerClass();
    if (!base) return false;
    void* const cls = ue_wrap::object_index::ClassByName(r.className.c_str());
    return cls && ue_wrap::prop::WalksToBase(cls, base);
}

bool CarriesInventoryIndex(const SR::SaveRecord& r) {
    return !r.ints.empty() && !r.ints[0].empty() && r.ints[0][0] != -1;
}

void ClearInventoryIndex(SR::SaveRecord& r) {
    if (r.ints.empty()) r.ints.resize(1);
    if (r.ints[0].empty()) r.ints[0].resize(1);
    r.ints[0][0] = -1;
}

// updateVolumesAndMass calls only Get Volume; the ejector checkObjectsVolume (which calls takeObj) is
// not called. Each verb is looked up on the instance's own class through the memoised dispatch
// lookup, which climbs to the class that declares it -- updateVolumesAndMass is declared only on
// Aprop_container_C, and every real container is a subclass -- and holds its answer by the class's
// slot and serial, so no function of a class that is gone is called.
void RederiveShownState(void* container, void* inventory) {
    void* const updateVol =
        container ? R::FindDispatchFunctionCached(R::ClassOf(container), L"updateVolumesAndMass") : nullptr;
    void* const recalcNames =
        inventory ? R::FindDispatchFunctionCached(R::ClassOf(inventory), L"recalculateNames") : nullptr;
    if ((container && !updateVol) || (inventory && !recalcNames)) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("container_inventory: re-derive verb MISSING (updateVolumesAndMass=%p recalculateNames=%p) "
                    "-- applied contents will show a STALE currVol / names", updateVol, recalcNames);
        }
    }
    if (updateVol)   ue_wrap::component_calls::CallParamless(container, updateVol);
    if (recalcNames) ue_wrap::component_calls::CallParamless(inventory, recalcNames);
}

bool CurrentVolume(void* inventory, float& out) {
    out = 0.f;
    if (!inventory || CachedOffset(g_offCurrVol, R::ClassOf(inventory), L"currVol") < 0) return false;
    out = ReadAt<float>(inventory, g_offCurrVol);
    return true;
}

}  // namespace ue_wrap::container_inventory
