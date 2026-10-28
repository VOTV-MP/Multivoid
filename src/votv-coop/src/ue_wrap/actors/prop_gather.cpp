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
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

constexpr uint64_t kRebuildThrottleMs = 3000;   // a failed read is retried no sooner than this
// A sanity bound on the save slot's equipment TArray count, past which it is not a count; not the game's cap.
constexpr int32_t  kMaxEquipment = 1024;

// The classes whose own getData returns an empty struct (the Super's result never reaches its out param, or a
// default struct is assigned), so the game refuses to save, and so to pocket or hold, them.
constexpr const wchar_t* kEmptySaveClasses[] = {L"prop_burgerHallucinate_C", L"prop_inventoryContainer_player_C",
                                                L"prop_rock_C", L"prop_drive_ae_C"};

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A read that failed is not tried again for the throttle window, so an absent object or member costs one walk
// per window and not one per request.
struct FailLatch {
    bool     failed = false;
    uint64_t atMs = 0;
    bool Blocked() const { return failed && NowMs() - atMs < kRebuildThrottleMs; }
    void Fail() {
        failed = true;
        atMs = NowMs();
    }
    void Clear() { failed = false; }
};

// The UFunction an instance of the prop's class dispatches for the row's name. Memoised per class (a class that
// dies with its world is resolved again), as these run on every judged request.
void* DispatchFn(const facts::Function& row, void* prop) {
    return R::FindDispatchFunctionCached(R::ClassOf(prop), row.Name());
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
FailLatch       g_propDataFail;

// The three bools sit inside the Fstruct_prop at prop + Aprop_propData; their offsets in the struct come from
// the struct itself, by name prefix (the GUID suffix moves with a recook). Game thread.
bool ResolvePropData() {
    UE_ASSERT_GAME_THREAD("prop_gather::ResolvePropData");
    if (g_propData.ok) return true;
    if (g_propDataFail.Blocked()) return false;
    void* cls = R::FindClass(L"prop_C");
    void* st = cls ? R::PropertyInnerStruct(cls, L"propData") : nullptr;
    PropDataOffsets o;
    if (st) {
        o.canCollect = R::FindPropertyOffsetByPrefix(st, L"canCollect_");
        o.canHold = R::FindPropertyOffsetByPrefix(st, L"canHold_");
        o.heavy = R::FindPropertyOffsetByPrefix(st, L"heavy_");
    }
    if (!st || o.canCollect < 0 || o.canHold < 0 || o.heavy < 0) {
        g_propDataFail.Fail();
        return false;
    }
    o.ok = true;
    g_propData = o;
    return true;
}

// ---- list_props and list_weapons ---------------------------------------------------------------

// A DataTable is an asset that outlives a world, so a set built from one is kept until its table is no longer
// alive. SOFT: the table or a reflected path is not resolvable yet, tried again no sooner than the throttle (its
// lookup walks every object). HARD: a verdict about the layout or the data (a member missing, duplicate keys,
// the column gate disagreeing), which no retry changes -- weapon_catalog's outcomes.
enum class Outcome { Never, Soft, Hard, Valid };

struct Table {
    const wchar_t*        name;
    ue_wrap::CachedObjRef ref;
    Outcome               outcome = Outcome::Never;
    uint64_t              lastAttemptMs = 0;
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

// Rows whose heavy flag is known from the data table's own content (lowercased names): the layout check reads them
// through the same raw member reads the judge uses.
struct LayoutRow {
    const wchar_t* name;
    bool           heavy;
};
constexpr LayoutRow kLayoutRows[] = {{L"heater", true}, {L"crowbar", false}};

Outcome BuildProps(Table& t) {
    void* table = TableObject(t);
    if (!table) return Outcome::Soft;
    void* rowStruct = DT::RowStruct(table);
    if (!rowStruct) {
        UE_LOGE("prop_gather: list_props' RowStruct did not resolve");
        return Outcome::Soft;
    }
    const int32_t offCollect = R::FindPropertyOffsetByPrefix(rowStruct, L"canCollect_");
    const int32_t offHold = R::FindPropertyOffsetByPrefix(rowStruct, L"canHold_");
    const int32_t offHeavy = R::FindPropertyOffsetByPrefix(rowStruct, L"heavy_");
    const int32_t offMesh = R::FindPropertyOffsetByPrefix(rowStruct, L"mesh_");
    if (offCollect < 0 || offHold < 0 || offHeavy < 0 || offMesh < 0) {
        UE_LOGE("prop_gather: list_props' row members did not resolve (canCollect@%d canHold@%d heavy@%d mesh@%d) "
                "-- the set is INVALID", offCollect, offHold, offHeavy, offMesh);
        return Outcome::Hard;
    }
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "prop_gather")) return Outcome::Soft;
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
        UE_LOGE("prop_gather: list_props' %zu rows collapsed to %zu keys -- duplicate row names, the set is INVALID",
                refs.size(), rows.size());
        return Outcome::Hard;
    }
    // The layout gate: a Hard verdict is final, so each line below is said once.
    for (const LayoutRow& lr : kLayoutRows) {
        const auto it = rows.find(lr.name);
        if (it == rows.end()) {
            UE_LOGE("prop_gather: list_props has no row '%ls' to check the layout against -- the set is INVALID",
                    lr.name);
            return Outcome::Hard;
        }
        if (it->second.flags.heavy != lr.heavy) {
            UE_LOGE("prop_gather: list_props row '%ls' reads heavy=%d, expected %d -- the RowMap layout assumption is "
                    "wrong on this build; the set is INVALID", lr.name, it->second.flags.heavy ? 1 : 0, lr.heavy ? 1 : 0);
            return Outcome::Hard;
        }
    }
    g_propRows = std::move(rows);
    UE_LOGI("prop_gather: list_props read, %zu rows, layout checked on rows heater and crowbar", g_propRows.size());
    return Outcome::Valid;
}

Outcome BuildWeapons(Table& t) {
    void* table = TableObject(t);
    if (!table) return Outcome::Soft;
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "prop_gather")) return Outcome::Soft;
    std::unordered_set<std::wstring> keys;
    keys.reserve(refs.size());
    for (const DT::RowRef& ref : refs) keys.insert(Lower(R::ToString(ref.key)));
    g_weaponKeys = std::move(keys);
    UE_LOGI("prop_gather: list_weapons read, %zu rows", g_weaponKeys.size());
    return Outcome::Valid;
}

// The set is built at the first query and again only when its table is no longer alive; a Hard verdict is final.
bool Ready(Table& t, Outcome (*build)(Table&)) {
    if (t.outcome == Outcome::Valid && t.ref.Alive()) return true;
    if (t.outcome == Outcome::Hard) return false;
    const uint64_t now = NowMs();
    if (t.outcome == Outcome::Soft && now - t.lastAttemptMs < kRebuildThrottleMs) return false;
    t.outcome = build(t);
    t.lastAttemptMs = now;
    return t.outcome == Outcome::Valid;
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

// A BP function with one out bool, called on the prop through its row: TRUE refuses. False when it did not run or
// its out param is not in the frame (an unread fact is never "not refused").
bool AskRefusal(const facts::Function& row, void* prop, const wchar_t* outName, bool& refused) {
    void* const fn = DispatchFn(row, prop);
    if (!fn) return false;
    ParamFrame f(fn);
    bool out = false;
    if (!f.valid() || !Call(prop, f) || !f.GetRaw(outName, &out, static_cast<int32_t>(sizeof out))) return false;
    refused = out;
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
FailLatch             g_libFail;

bool EnsureLib() {
    if (g_libCdo.Alive() && g_getVolume) return true;
    if (g_libFail.Blocked()) return false;
    if (!g_libCdo.Alive()) {
        g_libCdo.Set(R::FindClassDefaultObject(L"lib_C"));
        g_getVolume = {};
    }
    if (g_libCdo.Alive() && !g_getVolume) g_getVolume = facts::Resolve(kRowGetVolume);
    if (g_libCdo.Alive() && g_getVolume) {
        g_libFail.Clear();
        return true;
    }
    g_libFail.Fail();
    return false;
}

// The volume the game's Get Volume answers for `mesh`, as addObject asks it (the name left None). A prop with no
// mesh adds none. The function's `valid` out is ignored, as the game ignores it.
bool VolumeOfMesh(void* prop, void* mesh, float& volume) {
    volume = 0.f;
    if (!mesh) return true;
    if (!EnsureLib()) return false;
    ParamFrame f(g_getVolume.Fn());
    float out = 0.f;
    if (!f.valid() || !f.Set<void*>(L"mesh", mesh) || !f.Set<void*>(L"__WorldContext", prop) ||
        !Call(g_libCdo.Get(), f) || !f.GetRaw(L"volume", &out, static_cast<int32_t>(sizeof out)))
        return false;
    volume = out;
    return true;
}

// The largest container any player can reach holds (500 + 250 * fannypacks) * 100, and the most fannypacks a
// player can wear is the length of the save slot's equipment array, which the game never resizes. Read once.
bool Bound(float& bound) {
    static float     s_bound = -1.f;
    static FailLatch s_fail;
    if (s_bound < 0.f) {
        if (s_fail.Blocked()) return false;
        void* cdo = R::FindClassDefaultObject(L"saveSlot_C");
        void* cls = cdo ? R::ClassOf(cdo) : nullptr;
        const int32_t off = cls ? R::FindPropertyOffset(cls, L"equipment") : -1;
        const auto* arr = off < 0 ? nullptr
                                  : reinterpret_cast<const field_io::TArrayView*>(static_cast<const uint8_t*>(cdo) + off);
        if (!arr || arr->num <= 0 || arr->num > kMaxEquipment) {
            s_fail.Fail();
            return false;
        }
        s_bound = (500.f + 250.f * static_cast<float>(arr->num)) * 100.f;
    }
    bound = s_bound;
    return true;
}

// ---- one prop ----------------------------------------------------------------------------------

// The flags a gather reads, and the mesh its volume is taken from. A frozen prop is read the way the game reads
// it after awakeUnfreeze ran init(): from its class's list_props row by name, and a class with no row is
// collectable, holdable and not heavy, with no mesh. Any other prop is read from its live struct and its root.
// `lowerName` is the prop's lowercased name, read only for a frozen prop.
bool FlagsAndMesh(void* prop, bool frozen, void* root, const std::wstring& lowerName, PropDataFlags& flags,
                  void*& mesh) {
    mesh = nullptr;
    if (!frozen) {
        if (!ReadPropData(prop, flags)) return false;
        mesh = IsStaticMeshComponent(root) ? RootMesh(root) : nullptr;
        return true;
    }
    bool found = false;
    PropDataFlags row{};
    void* rowMesh = nullptr;
    if (!ListPropsRow(lowerName, row, &rowMesh, &found)) return false;
    if (found) {
        flags = row;
        mesh = rowMesh;
    } else {
        flags = PropDataFlags{true, true, false};
    }
    return true;
}

// Whether `cls` or one of its supers is named one of `names` (an FName compares without case). The prop's own class
// chain is compared by name, so no class is looked up: one that is not loaded has no instance to judge. The hop
// bound is the tree's SuperStruct walk's.
bool ClassChainNamed(void* cls, const wchar_t* const* names, size_t count) {
    for (int hops = 0; hops < 16 && cls; ++hops, cls = R::SuperStructOf(cls))
        for (size_t i = 0; i < count; ++i)
            if (R::NameEquals(R::NameOf(cls), names[i])) return true;
    return false;
}

bool IsEmptySaveClass(void* prop) {
    return ClassChainNamed(R::ClassOf(prop), kEmptySaveClasses, std::size(kEmptySaveClasses));
}

// A hose's getData writes nothing while its `placing` flag is true, so the game refuses to save, pocket or hold one
// being placed; a hose whose flag cannot be read counts as placing (fail closed), and is read again no sooner than
// the throttle. `placing` is read on the host's copy, the authority for the placement it judges.
constexpr const wchar_t* kHoseClass = L"prop_hose_C";
struct PlacingField {
    ue_wrap::CachedObjRef cls;
    int32_t               off = -1;
    uint8_t               mask = 0;
    bool                  ok = false;
    FailLatch             fail;
};
// One entry per hose class (prop_hose_C and prop_hose_grab_C), side by side; a further class replaces the oldest.
PlacingField g_placing[2];
size_t       g_placingNext = 0;

PlacingField& PlacingOf(void* cls) {
    for (PlacingField& e : g_placing)
        if (e.cls.Is(cls)) return e;
    PlacingField& e = g_placing[g_placingNext++ % std::size(g_placing)];
    e.cls.Set(cls);
    e.ok = false;
    e.fail = FailLatch{};
    return e;
}

bool IsHosePlacing(void* prop) {
    void* const cls = R::ClassOf(prop);
    if (!ClassChainNamed(cls, &kHoseClass, 1)) return false;
    PlacingField& e = PlacingOf(cls);
    if (!e.ok && !e.fail.Blocked()) {
        e.ok = R::FindBoolProperty(cls, L"placing", e.off, e.mask);
        if (e.ok) e.fail.Clear();
        else e.fail.Fail();
    }
    return !e.ok || (*(static_cast<const uint8_t*>(prop) + e.off) & e.mask) != 0;
}

bool LiveProp(void* prop) { return prop && R::IsLive(prop); }

}  // namespace

bool ReadPropData(void* prop, PropDataFlags& out) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadPropData");
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
    const bool frozen = prop::IsFrozen(prop);
    const std::wstring name = frozen ? Lower(prop::GetPropNameString(prop)) : std::wstring();
    return FlagsAndMesh(prop, frozen, engine::RootComponentOf(prop), name, flags, mesh) &&
           VolumeOfMesh(prop, mesh, volume) && Bound(bound);
}

bool ReadPocketFacts(void* prop, PocketFacts& out) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadPocketFacts");
    out = PocketFacts{};
    if (!LiveProp(prop)) return false;
    PocketFacts f{};
    if (!AskRefusal(kRowCanBeCollected, prop, L"ignore", f.collectRefused)) return false;
    bool childRead = false;
    f.child = engine::IsChildActor(prop, childRead);
    if (!childRead) return false;
    f.frozen = prop::IsFrozen(prop);
    void* root = engine::RootComponentOf(prop);   // read once: the physics test and the mesh both use it
    f.simulating = root && engine::IsComponentSimulatingPhysics(root);
    PropDataFlags flags;
    void* mesh = nullptr;
    const std::wstring name = f.frozen ? Lower(prop::GetPropNameString(prop)) : std::wstring();
    if (!FlagsAndMesh(prop, f.frozen, root, name, flags, mesh)) return false;
    f.canCollect = flags.canCollect;
    f.heavy = flags.heavy;
    f.meshRoot = IsStaticMeshComponent(root);
    float volume = 0.f, bound = 0.f;
    if (!VolumeOfMesh(prop, mesh, volume) || !Bound(bound)) return false;
    f.volumeFits = volume <= bound;
    f.saveClass = !IsEmptySaveClass(prop) && !IsHosePlacing(prop);
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
    bool childRead = false;
    f.child = engine::IsChildActor(prop, childRead);
    if (!childRead) return false;
    f.frozen = prop::IsFrozen(prop);
    f.simulating = engine::IsActorRootSimulatingPhysics(prop);
    PropDataFlags flags;
    void* mesh = nullptr;
    const std::wstring name = Lower(prop::GetPropNameString(prop));   // once: the row lookup and the equip row use it
    if (!FlagsAndMesh(prop, f.frozen, nullptr, name, flags, mesh)) return false;
    f.canHold = flags.canHold;
    f.heavy = flags.heavy;
    f.saveClass = !IsEmptySaveClass(prop) && !IsHosePlacing(prop);
    if (!HasEquipRow(name, &f.equipRow)) return false;
    out = f;
    return true;
}

bool ReadRowFlags(const std::wstring& name, PropDataFlags& out, bool& found) {
    UE_ASSERT_GAME_THREAD("prop_gather::ReadRowFlags");
    out = PropDataFlags{};
    found = false;
    void* mesh = nullptr;
    return ListPropsRow(Lower(name), out, &mesh, &found);
}

namespace {

// The game's own name -> volume table: the gamemode's propRenderer names (TArray<FName>) beside the gamemode's
// prop_volumes (TArray<float>), which Get Volume with no mesh reads as prop_volumes[Array_Find(names, name)]. It is
// read into a map by lowercased name (an FName compares without case; the first name wins, as Array_Find does) and read
// again when the renderer or either array's count changes. A name the table lacks is unknown with no engine call.
struct VolumeTable {
    void* gmCls = nullptr;       // the gamemode class the offsets below were read from
    int32_t renderer = -1;       // mainGamemode_C.propRenderer
    int32_t volumes = -1;        // mainGamemode_C.prop_volumes
    void* rendererCls = nullptr;
    int32_t names = -1;          // propProcessor_C.names
    void* builtFor = nullptr;    // the renderer the map was built from
    int32_t namesNum = -1;
    int32_t volumesNum = -1;
    std::unordered_map<std::wstring, float> byName;
};
VolumeTable g_volumeTable;

void RebuildVolumeTable(VolumeTable& t, const save_record::Arr& names, const save_record::Arr& volumes) {
    t.byName.clear();
    t.byName.reserve(static_cast<size_t>(names.num));
    for (int32_t i = 0; i < names.num && i < volumes.num; ++i) {
        R::FName n{};
        std::memcpy(&n, names.data + static_cast<size_t>(i) * sizeof(R::FName), sizeof n);
        float v = 0.f;
        std::memcpy(&v, volumes.data + static_cast<size_t>(i) * sizeof v, sizeof v);
        t.byName.emplace(Lower(R::ToString(n)), v);   // emplace keeps the first of a repeated name
    }
}

}  // namespace

bool VolumeOfName(const std::wstring& name, float& out, bool& valid) {
    UE_ASSERT_GAME_THREAD("prop_gather::VolumeOfName");
    out = 0.f;
    valid = false;
    void* const gm = world_singleton::Gamemode();
    if (!gm) return false;
    VolumeTable& t = g_volumeTable;
    if (void* const gmCls = R::ClassOf(gm); gmCls != t.gmCls) {
        t.gmCls = gmCls;
        t.renderer = R::FindPropertyOffset(gmCls, L"propRenderer");
        t.volumes = R::FindPropertyOffset(gmCls, L"prop_volumes");
    }
    if (t.renderer < 0 || t.volumes < 0) return false;
    void* const renderer = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + t.renderer);
    if (!renderer || !R::IsLive(renderer)) return false;
    if (void* const rCls = R::ClassOf(renderer); rCls != t.rendererCls) {
        t.rendererCls = rCls;
        t.names = R::FindPropertyOffset(rCls, L"names");
    }
    if (t.names < 0) return false;
    const save_record::Arr names = save_record::ReadArr(renderer, t.names);
    const save_record::Arr volumes = save_record::ReadArr(gm, t.volumes);
    if (names.num <= 0 || volumes.num <= 0) return false;   // the game has not filled its table yet
    if (renderer != t.builtFor || names.num != t.namesNum || volumes.num != t.volumesNum) {
        RebuildVolumeTable(t, names, volumes);
        t.builtFor = renderer;
        t.namesNum = names.num;
        t.volumesNum = volumes.num;
    }
    const auto it = t.byName.find(Lower(name));
    if (it == t.byName.end()) return true;   // unknown: valid stays false
    out = it->second;
    valid = true;
    return true;
}

}  // namespace ue_wrap::prop_gather
