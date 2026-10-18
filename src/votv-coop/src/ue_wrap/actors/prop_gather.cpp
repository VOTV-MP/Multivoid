// ue_wrap/actors/prop_gather.cpp -- see ue_wrap/actors/prop_gather.h.
//
// Every read here is pure: the BP functions it calls (canBeCollected, canBePickedUp, ignoreSave, Get Volume)
// only answer, and getData, which can add components or write save state, is never called. A frozen prop's
// flags come from its list_props row, the row the game's init() resets a frozen prop from before it reads them.

#include "ue_wrap/actors/prop_gather.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ue_wrap::prop_gather {

// The functions the reads call on the prop: a Dispatch row is resolved on the prop's own class, so an override
// (a battery that is plugged in, a pot with a plant) answers instead of prop_C's.
UE_FACT_FN(kRowCanBeCollected, "prop_act", L"prop_C", L"canBeCollected", Dispatch);
UE_FACT_FN(kRowCanBePickedUp, "prop_act", L"prop_C", L"canBePickedUp", Dispatch);
UE_FACT_FN(kRowIgnoreSave, "prop_act", L"prop_C", L"ignoreSave", Dispatch);
// lib_C's pure volume lookup by mesh, the call addObject makes for the volume it adds.
UE_FACT_FN(kRowGetVolume, "prop_act", L"lib_C", L"Get Volume", Declared);

namespace {

namespace P  = profile;
namespace R  = reflection;
namespace DT = data_table;

constexpr uint64_t kRebuildThrottleMs = 3000;   // a failed table build is retried no sooner than this
constexpr int32_t  kMaxEquipment = 1024;        // a TArray count past this is not a count

// The classes whose getData returns an empty struct, so the game refuses to save, and so to pocket, them.
constexpr const wchar_t* kEmptySaveClasses[] = {L"prop_burgerHallucinate_C", L"prop_inventoryContainer_player_C"};
ue_wrap::CachedObjRef g_emptySaveClasses[std::size(kEmptySaveClasses)];

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lowercased: an FName compares case-insensitively.
std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

// ---- propData's members ------------------------------------------------------------------------

struct PropDataOffsets {
    int32_t canCollect = -1, canHold = -1, heavy = -1;
    bool    ok = false;
};
PropDataOffsets g_propData;

// The three bools sit inside the Fstruct_prop at prop + Aprop_propData; their offsets in the struct come from
// the struct itself, by name prefix (the GUID suffix moves with a recook).
bool ResolvePropData() {
    if (g_propData.ok) return true;
    void* cls = R::FindClass(L"prop_C");
    void* st = cls ? R::PropertyInnerStruct(cls, L"propData") : nullptr;
    if (!st) return false;
    PropDataOffsets o;
    o.canCollect = R::FindPropertyOffsetByPrefix(st, L"canCollect_");
    o.canHold = R::FindPropertyOffsetByPrefix(st, L"canHold_");
    o.heavy = R::FindPropertyOffsetByPrefix(st, L"heavy_");
    if (o.canCollect < 0 || o.canHold < 0 || o.heavy < 0) return false;
    o.ok = true;
    g_propData = o;
    return true;
}

// ---- list_props and list_weapons ---------------------------------------------------------------

// A DataTable is an asset that outlives a world, so a set built from one is kept until its table is no longer
// alive. A build that fails answers false, and is tried again no sooner than the throttle.
struct Table {
    const wchar_t*        name;
    ue_wrap::CachedObjRef ref;
    bool                  valid = false;
    bool                  failed = false;
    uint64_t              failedAtMs = 0;
};

struct PropRow {
    PropDataFlags flags;
    void*         mesh;   // the StaticMesh the row names, a hard reference; null when the row has none
};

Table g_propsTable{L"list_props", {}};
Table g_weaponsTable{L"list_weapons", {}};
std::unordered_map<std::wstring, PropRow> g_propRows;
std::unordered_set<std::wstring>          g_weaponKeys;

void* TableObject(Table& t) {
    if (!t.ref.Alive()) t.ref.Set(R::FindObject(t.name, L"DataTable"));
    return t.ref.Get();
}

bool BuildProps(Table& t) {
    void* table = TableObject(t);
    if (!table) return false;
    void* rowStruct = DT::RowStruct(table);
    if (!rowStruct) {
        UE_LOGE("prop_gather: list_props' RowStruct did not resolve");
        return false;
    }
    const int32_t offCollect = R::FindPropertyOffsetByPrefix(rowStruct, L"canCollect_");
    const int32_t offHold = R::FindPropertyOffsetByPrefix(rowStruct, L"canHold_");
    const int32_t offHeavy = R::FindPropertyOffsetByPrefix(rowStruct, L"heavy_");
    const int32_t offMesh = R::FindPropertyOffsetByPrefix(rowStruct, L"mesh_");
    if (offCollect < 0 || offHold < 0 || offHeavy < 0 || offMesh < 0) {
        UE_LOGE("prop_gather: list_props' row members did not resolve (canCollect@%d canHold@%d heavy@%d mesh@%d)",
                offCollect, offHold, offHeavy, offMesh);
        return false;
    }
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "prop_gather")) return false;
    std::unordered_map<std::wstring, PropRow> rows;
    rows.reserve(refs.size());
    for (const DT::RowRef& ref : refs) {
        PropRow r;
        r.flags.canCollect = ref.row[offCollect] != 0;
        r.flags.canHold = ref.row[offHold] != 0;
        r.flags.heavy = ref.row[offHeavy] != 0;
        r.mesh = *reinterpret_cast<void* const*>(ref.row + offMesh);
        rows.emplace(Lower(R::ToString(ref.key)), r);
    }
    if (rows.size() != refs.size()) {
        UE_LOGE("prop_gather: list_props' %zu rows collapsed to %zu keys -- duplicate row names", refs.size(),
                rows.size());
        return false;
    }
    g_propRows = std::move(rows);
    UE_LOGI("prop_gather: list_props read, %zu rows", g_propRows.size());
    return true;
}

bool BuildWeapons(Table& t) {
    void* table = TableObject(t);
    if (!table) return false;
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "prop_gather")) return false;
    std::unordered_set<std::wstring> keys;
    keys.reserve(refs.size());
    for (const DT::RowRef& ref : refs) keys.insert(Lower(R::ToString(ref.key)));
    g_weaponKeys = std::move(keys);
    UE_LOGI("prop_gather: list_weapons read, %zu rows", g_weaponKeys.size());
    return true;
}

// The set is built at the first query and again only when its table is no longer alive.
bool Ready(Table& t, bool (*build)(Table&)) {
    if (t.valid && t.ref.Alive()) return true;
    const uint64_t now = NowMs();
    if (t.failed && now - t.failedAtMs < kRebuildThrottleMs) return false;
    t.valid = build(t);
    t.failed = !t.valid;
    t.failedAtMs = now;
    return t.valid;
}

// `name` is lowercased. False: the table could not be read. `found` says whether it has the row; `rowMesh` is the
// row's mesh when it does.
bool ListPropsRow(const std::wstring& name, PropDataFlags& flags, void** rowMesh, bool* found) {
    if (!Ready(g_propsTable, BuildProps)) return false;
    const auto it = g_propRows.find(name);
    *found = it != g_propRows.end();
    if (*found) {
        flags = it->second.flags;
        *rowMesh = it->second.mesh;
    }
    return true;
}

// Does the game's addEquip find a row by `name` (lowercased) in list_weapons or list_props? False: unreadable.
bool HasEquipRow(const std::wstring& name, bool* has) {
    if (!Ready(g_weaponsTable, BuildWeapons) || !Ready(g_propsTable, BuildProps)) return false;
    *has = g_weaponKeys.count(name) != 0 || g_propRows.count(name) != 0;
    return true;
}

// ---- the BP answers ----------------------------------------------------------------------------

// A BP function with one out bool, called on the prop through its row: TRUE refuses. False when it did not run.
bool AskRefusal(const facts::Function& row, void* prop, const wchar_t* outName, bool& refused) {
    const facts::Resolved fn = facts::ResolveOn(row, R::ClassOf(prop));
    if (!fn) return false;
    ParamFrame f(fn.Fn());
    if (!f.valid() || !Call(prop, f)) return false;
    refused = f.Get<bool>(outName);
    return true;
}

// ---- the root's mesh and the volume ------------------------------------------------------------

void* StaticMeshComponentClass() {
    static void* s_cls = nullptr;   // an engine class: it never reloads
    if (!s_cls) s_cls = R::FindClass(L"StaticMeshComponent");
    return s_cls;
}

bool IsStaticMeshComponent(void* comp) {
    void* smc = StaticMeshComponentClass();
    return comp && smc && prop::WalksToBase(R::ClassOf(comp), smc);
}

void* RootMesh(void* root) {
    static int32_t s_off = -1;
    if (s_off < 0) {
        void* smc = StaticMeshComponentClass();
        s_off = smc ? R::FindPropertyOffset(smc, L"StaticMesh") : -1;
    }
    if (s_off < 0 || !root) return nullptr;
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(root) + s_off);
}

ue_wrap::CachedObjRef g_libCdo;
facts::Resolved       g_getVolume;

bool EnsureLib() {
    if (!g_libCdo.Alive()) {
        g_libCdo.Set(R::FindClassDefaultObject(L"lib_C"));
        g_getVolume = {};
    }
    if (g_libCdo.Alive() && !g_getVolume) g_getVolume = facts::Resolve(kRowGetVolume);
    return g_libCdo.Alive() && g_getVolume;
}

// The volume the game's Get Volume answers for `mesh`, as addObject asks it (the name left None). A prop with no
// mesh adds none. The function's `valid` out is ignored, as the game ignores it.
bool VolumeOfMesh(void* prop, void* mesh, float& volume) {
    volume = 0.f;
    if (!mesh) return true;
    if (!EnsureLib()) return false;
    ParamFrame f(g_getVolume.Fn());
    if (!f.valid() || !f.Set<void*>(L"mesh", mesh) || !f.Set<void*>(L"__WorldContext", prop) ||
        !Call(g_libCdo.Get(), f))
        return false;
    volume = f.Get<float>(L"volume");
    return true;
}

// The largest container any player can reach holds (500 + 250 * fannypacks) * 100, and the most fannypacks a
// player can wear is the length of the save slot's equipment array, which the game never resizes. Read once.
bool Bound(float& bound) {
    static float s_bound = -1.f;
    if (s_bound < 0.f) {
        void* cdo = R::FindClassDefaultObject(L"saveSlot_C");
        void* cls = cdo ? R::ClassOf(cdo) : nullptr;
        const int32_t off = cls ? R::FindPropertyOffset(cls, L"equipment") : -1;
        if (off < 0) return false;
        const auto* arr = reinterpret_cast<const field_io::TArrayView*>(static_cast<const uint8_t*>(cdo) + off);
        if (arr->num <= 0 || arr->num > kMaxEquipment) return false;
        s_bound = (500.f + 250.f * static_cast<float>(arr->num)) * 100.f;
    }
    bound = s_bound;
    return true;
}

// ---- one prop ----------------------------------------------------------------------------------

// The flags a gather reads, and the mesh its volume is taken from. A frozen prop is read the way the game reads
// it after awakeUnfreeze ran init(): from its class's list_props row by name, and a class with no row is
// collectable, holdable and not heavy, with no mesh. Any other prop is read from its live struct and its root.
bool FlagsAndMesh(void* prop, bool frozen, void* root, PropDataFlags& flags, void*& mesh) {
    mesh = nullptr;
    if (!frozen) {
        if (!ReadPropData(prop, flags)) return false;
        mesh = IsStaticMeshComponent(root) ? RootMesh(root) : nullptr;
        return true;
    }
    bool found = false;
    PropDataFlags row{};
    void* rowMesh = nullptr;
    if (!ListPropsRow(Lower(prop::GetPropNameString(prop)), row, &rowMesh, &found)) return false;
    if (found) {
        flags = row;
        mesh = rowMesh;
    } else {
        flags = PropDataFlags{true, true, false};
    }
    return true;
}

bool IsEmptySaveClass(void* prop) {
    void* cls = R::ClassOf(prop);
    for (size_t i = 0; i < std::size(kEmptySaveClasses); ++i) {
        ue_wrap::CachedObjRef& ref = g_emptySaveClasses[i];
        if (!ref.Alive()) ref.Set(object_index::ClassByName(kEmptySaveClasses[i]));
        if (ref.Raw() && prop::WalksToBase(cls, ref.Raw())) return true;
    }
    return false;
}

bool LiveProp(void* prop) { return prop && R::IsLive(prop); }

}  // namespace

bool ReadPropData(void* prop, PropDataFlags& out) {
    out = PropDataFlags{};
    if (!LiveProp(prop) || !ResolvePropData()) return false;
    const auto* data = static_cast<const uint8_t*>(prop) + P::off::Aprop_propData;
    out.canCollect = data[g_propData.canCollect] != 0;
    out.canHold = data[g_propData.canHold] != 0;
    out.heavy = data[g_propData.heavy] != 0;
    return true;
}

bool IsGatherable(void* prop) {
    return prop && !engine::IsChildActor(prop) && (prop::IsFrozen(prop) || engine::IsActorRootSimulatingPhysics(prop));
}

bool ReadVolumeAndBound(void* prop, float& volume, float& bound) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadVolumeAndBound");
    volume = bound = 0.f;
    if (!LiveProp(prop)) return false;
    PropDataFlags flags;
    void* mesh = nullptr;
    return FlagsAndMesh(prop, prop::IsFrozen(prop), engine::RootComponentOf(prop), flags, mesh) &&
           VolumeOfMesh(prop, mesh, volume) && Bound(bound);
}

bool ReadPocketFacts(void* prop, PocketFacts& out) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadPocketFacts");
    out = PocketFacts{};
    if (!LiveProp(prop)) return false;
    PocketFacts f{};
    if (!AskRefusal(kRowCanBeCollected, prop, L"ignore", f.collectRefused)) return false;
    f.child = engine::IsChildActor(prop);
    f.frozen = prop::IsFrozen(prop);
    f.simulating = engine::IsActorRootSimulatingPhysics(prop);
    void* root = engine::RootComponentOf(prop);
    PropDataFlags flags;
    void* mesh = nullptr;
    if (!FlagsAndMesh(prop, f.frozen, root, flags, mesh)) return false;
    f.canCollect = flags.canCollect;
    f.heavy = flags.heavy;
    f.meshRoot = IsStaticMeshComponent(root);
    float volume = 0.f, bound = 0.f;
    if (!VolumeOfMesh(prop, mesh, volume) || !Bound(bound)) return false;
    f.volumeFits = volume <= bound;
    f.saveClass = !IsEmptySaveClass(prop);
    out = f;
    return true;
}

bool ReadHoldFacts(void* prop, HoldFacts& out) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadHoldFacts");
    out = HoldFacts{};
    if (!LiveProp(prop)) return false;
    HoldFacts f{};
    if (!AskRefusal(kRowCanBePickedUp, prop, L"ignore", f.pickupRefused) ||
        !AskRefusal(kRowIgnoreSave, prop, L"ignoreSave", f.ignoreSave))
        return false;
    f.child = engine::IsChildActor(prop);
    f.frozen = prop::IsFrozen(prop);
    f.simulating = engine::IsActorRootSimulatingPhysics(prop);
    PropDataFlags flags;
    void* mesh = nullptr;
    if (!FlagsAndMesh(prop, f.frozen, nullptr, flags, mesh)) return false;
    f.canHold = flags.canHold;
    f.heavy = flags.heavy;
    f.saveClass = !IsEmptySaveClass(prop);
    if (!HasEquipRow(Lower(prop::GetPropNameString(prop)), &f.equipRow)) return false;
    out = f;
    return true;
}

}  // namespace ue_wrap::prop_gather
