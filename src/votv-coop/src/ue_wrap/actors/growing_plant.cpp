// ue_wrap/actors/growing_plant.cpp -- see ue_wrap/actors/growing_plant.h.

#include "ue_wrap/actors/growing_plant.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ue_wrap::growing_plant {
namespace {

namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace WI = ue_wrap::world_identity;
namespace DT = ue_wrap::data_table;

// The family is every class whose super chain reaches growingPlant_C: ForEachInstance walks one exact class, and a list of
// the game's classes by hand goes stale with the next game build. The chain is walked by pointer against the base class, found
// once through the object index and found again when its listing moves (the class unloaded or loaded again): no name is
// rendered per hop. The table holds the classes that have a live instance, and is rebuilt when the object index's class set
// moves (a class gained its first instance or lost its last), the world changes or the base is a different class object.
constexpr const wchar_t* kBase = L"growingPlant_C";
constexpr int kMaxHops = 32;

struct Family {
    std::vector<void*> classes;   // sorted, so a rebuild can tell whether the set moved
    uint64_t classSet = 0;        // the object index's class set at the last build
    uint32_t world = 0;           // the world generation of the last build; 0 is none built
    uint64_t baseListing = 0;     // the base's listing at the last build; 0 is none
};
Family g_family;

// growingPlant_C as the object index lists it, null while it is not loaded.
void* BaseClass(uint64_t* listing) {
    static void* s_base = nullptr;
    static uint64_t s_listing = 0;
    if (!s_base || OI::ClassListingId(s_base) != s_listing) {
        s_base = OI::ClassByName(kBase);
        s_listing = s_base ? OI::ClassListingId(s_base) : 0;
    }
    if (listing) *listing = s_listing;
    return s_base;
}

// Game thread, like every ForEachInstance. A line names the classes when their set differs from the last build's or the world
// is a new one: the class set moves often, and a line at every move would only repeat itself.
void RefreshFamily() {
    const uint64_t classSet = OI::ClassSetVersion();
    const uint32_t world = WI::Generation();
    uint64_t baseListing = 0;
    void* base = BaseClass(&baseListing);
    if (g_family.world == world && g_family.classSet == classSet && g_family.baseListing == baseListing) return;
    const bool newWorld = g_family.world != world;
    g_family.world = world;
    g_family.classSet = classSet;
    g_family.baseListing = baseListing;
    std::vector<void*> found;
    if (base) {
        struct Walk { void* base; std::vector<void*>* out; } walk{base, &found};
        OI::ForEachClass([](void* ctx, void* cls, void*) {
            auto* w = static_cast<Walk*>(ctx);
            if (R::IsDescendantOfAny(cls, &w->base, 1, kMaxHops)) w->out->push_back(cls);
        }, &walk);
    }
    std::sort(found.begin(), found.end());
    if (found == g_family.classes && !newWorld) return;
    g_family.classes = std::move(found);
    std::wstring names;
    for (void* cls : g_family.classes) {
        if (!names.empty()) names += L", ";
        names += R::ToString(R::NameOf(cls));
    }
    UE_LOGI("growing_plant: %zu plant classes under %ls: %ls", g_family.classes.size(), kBase, names.c_str());
}

// Where one class keeps its fields: each lookup is a walk of the class's properties, and a poll reads every plant's. An
// instance's class pointer names that instance's class whatever else has lived at the address, and the family's fields are all
// inherited from one base, so an entry cannot read another class's layout.
struct BoolAt {
    int32_t off = -1;
    uint8_t mask = 0;
};
struct Offsets {
    int32_t grow = -1, fertilizer = -1, water = -1, fruitGrow = -1, dryness = -1, amount = -1;
    int32_t plant = -1, attachParent = -1;   // FName fields
    BoolAt hideDirt, grew;
    int32_t plantType = -1, amountRange = -1;  // in the instance: its `data` row's type byte and crop range (two ints)
    bool Complete() const { return grow >= 0 && fertilizer >= 0 && water >= 0 && fruitGrow >= 0 && dryness >= 0 && amount >= 0; }
};
BoolAt FindBool(void* cls, const wchar_t* name) {
    BoolAt b;
    if (!R::FindBoolProperty(cls, name, b.off, b.mask)) b = BoolAt{};
    return b;
}

// struct_farmPlant's plantType: 1 a bush, 2 a tree; a harvest takes any other whole.
constexpr uint8_t kTypeBush = 1, kTypeTree = 2;

const Offsets& OffsetsOf(void* plant) {
    static std::unordered_map<void*, Offsets> s_byClass;
    void* cls = R::ClassOf(plant);
    auto it = s_byClass.find(cls);
    if (it != s_byClass.end()) return it->second;
    Offsets o;
    o.grow = R::FindPropertyOffset(cls, L"grow");
    o.fertilizer = R::FindPropertyOffset(cls, L"fertilizer");
    o.water = R::FindPropertyOffset(cls, L"water");
    o.fruitGrow = R::FindPropertyOffset(cls, L"fruitGrow");
    o.dryness = R::FindPropertyOffset(cls, L"dryness");
    o.amount = R::FindPropertyOffset(cls, L"amount");
    o.plant = R::FindPropertyOffset(cls, L"plant");
    o.attachParent = R::FindPropertyOffset(cls, L"attachParent");
    o.hideDirt = FindBool(cls, L"hideDirt");
    o.grew = FindBool(cls, L"grew");
    const int32_t data = R::FindPropertyOffset(cls, L"data");
    if (void* row = data >= 0 ? R::PropertyInnerStruct(cls, L"data") : nullptr) {
        const int32_t type = R::FindPropertyOffsetByPrefix(row, L"plantType_");
        const int32_t range = R::FindPropertyOffsetByPrefix(row, L"amount_range_");
        if (type >= 0) o.plantType = data + type;
        if (range >= 0) o.amountRange = data + range;
    }
    return s_byClass.emplace(cls, o).first->second;
}

template <class T>
T& At(void* plant, int32_t off) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(plant) + off);
}

template <class T>
T* Field(void* plant, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(plant), name);
    return off < 0 ? nullptr : reinterpret_cast<T*>(static_cast<uint8_t*>(plant) + off);
}

bool ReadBool(void* plant, const BoolAt& at) {
    return at.off >= 0 && (static_cast<uint8_t*>(plant)[at.off] & at.mask) != 0;
}

void WriteBool(void* plant, const BoolAt& at, bool on) {
    if (at.off < 0) return;
    uint8_t& b = static_cast<uint8_t*>(plant)[at.off];
    b = on ? (b | at.mask) : (b & ~at.mask);
}

std::wstring ReadName(void* plant, int32_t off) {
    return off < 0 ? std::wstring() : R::ToString(At<R::FName>(plant, off));
}

void WriteName(void* plant, const wchar_t* name, const std::wstring& value) {
    if (R::FName* f = Field<R::FName>(plant, name)) *f = fname_utils::StringToFName(value);
}

void CallWithBool(void* plant, const wchar_t* fnName, const wchar_t* param, bool value) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), fnName);
    if (!fn) return;
    ParamFrame f(fn);
    if (!f.valid()) return;
    f.Set<bool>(param, value);
    Call(plant, f);
}

void CallPlain(void* plant, const wchar_t* fnName) {
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), fnName)) component_calls::CallParamless(plant, fn);
}

}  // namespace

void List(std::vector<void*>& out) {
    RefreshFamily();
    for (void* cls : g_family.classes) {
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    }
}

bool IsPlantClass(void* cls) {
    void* base = BaseClass(nullptr);
    return base && R::IsDescendantOfAny(cls, &base, 1, kMaxHops);
}

bool IsPotClass(void* cls) {
    static void* s_pot = nullptr;
    static uint64_t s_listing = 0;
    if (!s_pot || OI::ClassListingId(s_pot) != s_listing) {
        s_pot = OI::ClassByName(L"prop_farmPot_C");
        s_listing = s_pot ? OI::ClassListingId(s_pot) : 0;
    }
    return s_pot && cls && R::IsDescendantOfAny(cls, &s_pot, 1, kMaxHops);
}

bool PotHoldsPlant(void* pot) {
    static std::unordered_map<void*, int32_t> s_offByClass;
    void* cls = R::ClassOf(pot);
    auto it = s_offByClass.find(cls);
    if (it == s_offByClass.end()) it = s_offByClass.emplace(cls, R::FindPropertyOffset(cls, L"plant")).first;
    if (it->second < 0) return true;   // a pot whose field is not found is not known to be empty
    void* held = At<void*>(pot, it->second);
    return held && R::IsLive(held);
}

std::wstring Key(void* plant) {
    std::wstring key = prop::GetActorSaveKeyString(plant);
    if (!key.empty() && key != L"None") return key;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), L"getKey");
    if (!fn) return L"";
    ParamFrame f(fn);
    if (!f.valid() || !Call(plant, f)) return L"";
    key = prop::GetActorSaveKeyString(plant);
    return key == L"None" ? L"" : key;
}

std::wstring Row(void* plant) { return ReadName(plant, OffsetsOf(plant).plant); }

namespace {

// What each row of list_farmPlants gives, by the row's lowercased name (an FName compares without case), read for the world
// of `g_farmRowsWorld`; 0 is none read. The row's plantObject is a hard class reference (struct_farmPlant: a ClassProperty,
// 8 bytes), so the table's load loaded every class in it. Its amount_range is an FIntPoint the game rolls a plant's berry
// count in; a row without the member (-1) bounds nothing.
struct FarmRow {
    void* cls = nullptr;
    int32_t amountMax = -1;
};
std::unordered_map<std::wstring, FarmRow> g_farmRowClasses;
uint32_t g_farmRowsWorld = 0;
uint64_t g_farmRowsTryMs = 0;
constexpr uint64_t kFarmRowsRetryMs = 3000;

std::wstring Lowered(const std::wstring& s) {
    std::wstring k = s;
    for (wchar_t& c : k)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return k;
}

bool ReadFarmRows() {
    const uint32_t world = WI::Generation();
    if (g_farmRowsWorld == world) return true;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (g_farmRowsTryMs != 0 && now - g_farmRowsTryMs < kFarmRowsRetryMs) return false;
    g_farmRowsTryMs = now;
    void* table = R::FindObject(L"list_farmPlants", L"DataTable");
    void* rowStruct = table ? DT::RowStruct(table) : nullptr;
    if (!rowStruct) return false;
    const int32_t off = R::FindPropertyOffsetByPrefix(rowStruct, L"plantObject_");
    if (off < 0) {
        UE_LOGE("growing_plant: list_farmPlants' row struct has no plantObject member");
        return false;
    }
    const int32_t rangeOff = R::FindPropertyOffsetByPrefix(rowStruct, L"amount_range_");
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "growing_plant") || refs.empty()) return false;
    g_farmRowClasses.clear();
    for (const DT::RowRef& ref : refs) {
        FarmRow row;
        row.cls = *reinterpret_cast<void* const*>(ref.row + off);
        if (rangeOff >= 0) {
            const int32_t* range = reinterpret_cast<const int32_t*>(ref.row + rangeOff);   // FIntPoint: X, Y
            row.amountMax = (std::max)({range[0], range[1], int32_t{0}});
        }
        g_farmRowClasses[Lowered(R::ToString(ref.key))] = row;
    }
    g_farmRowsWorld = world;
    UE_LOGI("growing_plant: list_farmPlants has %zu rows", g_farmRowClasses.size());
    return true;
}

}  // namespace

void* FarmRowPlantClass(const std::wstring& row) {
    if (row.empty() || !ReadFarmRows()) return nullptr;
    const auto it = g_farmRowClasses.find(Lowered(row));
    return (it != g_farmRowClasses.end() && it->second.cls && R::IsLive(it->second.cls)) ? it->second.cls : nullptr;
}

bool FarmRowAmountMax(const std::wstring& row, int32_t& out) {
    if (row.empty() || !ReadFarmRows()) return false;
    const auto it = g_farmRowClasses.find(Lowered(row));
    if (it == g_farmRowClasses.end() || it->second.amountMax < 0) return false;
    out = it->second.amountMax;
    return true;
}

bool FarmRowsReadable() { return ReadFarmRows(); }

bool IsFarmRow(const std::wstring& row) {
    return !row.empty() && ReadFarmRows() && g_farmRowClasses.count(Lowered(row)) != 0;
}

std::wstring AttachParent(void* plant) {
    std::wstring key = ReadName(plant, OffsetsOf(plant).attachParent);
    return key == L"None" ? L"" : key;
}

bool HideDirt(void* plant) { return ReadBool(plant, OffsetsOf(plant).hideDirt); }

bool Read(void* plant, Values& out) {
    const Offsets& o = OffsetsOf(plant);
    if (!o.Complete()) return false;
    out = Values{At<float>(plant, o.grow), At<float>(plant, o.fertilizer), At<float>(plant, o.water),
                 At<float>(plant, o.fruitGrow), At<float>(plant, o.dryness), At<int32_t>(plant, o.amount)};
    return true;
}

bool HarvestTakesPlant(void* plant) {
    const Offsets& o = OffsetsOf(plant);
    if (!o.Complete() || o.plantType < 0 || o.amountRange < 0) return false;
    const uint8_t type = At<uint8_t>(plant, o.plantType);
    // lookAt's discard: a range of nothing gives no crop.
    const bool crop = At<int32_t>(plant, o.amountRange) > 0 || At<int32_t>(plant, o.amountRange + 4) > 0;
    return type != kTypeBush && type != kTypeTree && At<float>(plant, o.grow) >= 1.f && crop;
}

void HarvestSound(void* plant) { CallPlain(plant, L"harvestSound"); }

void Apply(void* plant, const std::wstring& row, const Values& v) {
    Values old;
    if (!Read(plant, old)) return;
    const bool rowChanged = Row(plant) != row;
    const Offsets& o = OffsetsOf(plant);
    At<float>(plant, o.grow) = v.grow;
    At<float>(plant, o.fertilizer) = v.fertilizer;
    At<float>(plant, o.water) = v.water;
    At<float>(plant, o.fruitGrow) = v.fruitGrow;
    At<float>(plant, o.dryness) = v.dryness;
    At<int32_t>(plant, o.amount) = v.amount;
    if (rowChanged) WriteName(plant, L"plant", row);

    const bool nowGrown = v.grow >= 1.f;
    const bool grewEdge = nowGrown && !ReadBool(plant, o.grew);
    const bool fruitEdge = (old.fruitGrow >= 1.f) != (v.fruitGrow >= 1.f) || old.amount != v.amount;
    if (rowChanged || fruitEdge) CallWithBool(plant, L"upd", L"skipDataTable", !rowChanged);
    if (grewEdge || (rowChanged && nowGrown)) {
        WriteBool(plant, o.grew, true);
        CallWithBool(plant, L"processGrown", L"skipGrowCheck", true);
    } else if (!nowGrown && ReadBool(plant, o.grew)) {
        WriteBool(plant, o.grew, false);
    }
    CallPlain(plant, L"updLoc");
    // The refresh (upd, processGrown, updLoc) may set amount from its own read of the row: the sender's count stands. A move is
    // said once per row, so a run shows which refresh of which row moved it.
    const int32_t refreshed = At<int32_t>(plant, o.amount);
    if (refreshed != v.amount) {
        static std::unordered_set<std::wstring> s_said;
        if (s_said.insert(Lowered(row)).second)
            UE_LOGI("growing_plant: the game's refresh of a '%ls' plant set amount %d where the state says %d; the state's stands",
                    row.c_str(), refreshed, v.amount);
        At<int32_t>(plant, o.amount) = v.amount;
    }
}

void* Spawn(void* cls, const std::wstring& key, const std::wstring& row, const std::wstring& attachParent,
            bool hideDirt, const FVector& at, const FRotator& rot) {
    if (!cls) return nullptr;
    void* plant = engine::BeginDeferredSpawn(cls, at, rot);
    if (!plant) return nullptr;
    WriteName(plant, L"key", key);
    WriteName(plant, L"plant", row);
    WriteBool(plant, OffsetsOf(plant).hideDirt, hideDirt);
    if (!engine::FinishDeferredSpawn(plant, at, rot)) return nullptr;
    // Begin play clears this with no owner; processKeys finds the pot by it, attaches and joins the two.
    if (!attachParent.empty()) {
        WriteName(plant, L"attachParent", attachParent);
        if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), L"processKeys")) {
            ParamFrame f(fn);
            if (f.valid()) Call(plant, f);
        }
    }
    return plant;
}

bool Destroy(void* plant) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plant), L"K2_DestroyActor");
    return fn && component_calls::CallParamless(plant, fn);
}

}  // namespace ue_wrap::growing_plant
