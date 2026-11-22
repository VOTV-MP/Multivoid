// ue_wrap/actors/bucket.cpp -- see ue_wrap/actors/bucket.h.

#include "ue_wrap/actors/bucket.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"


namespace ue_wrap::bucket {
namespace {

namespace R = reflection;

// One member of a class: where it lives, and for a bool the bit inside its byte.
struct Field {
    int32_t off = -1;
    uint8_t mask = 0;   // 0 for a value member, the bit for a bool
};

struct FieldDef {
    const wchar_t* name;
    bool isBool;
};

constexpr size_t kMaxFields = 9;

// The members of one class, found together; `ok` only when every one resolved.
struct Layout {
    Field f[kMaxFields];
    bool ok = false;
};

enum PlainF : size_t { kHeight, kInterp, kMaxHeight, kSoap, kAlcoholP, kAlcohol, kAlcReady, kEmpty, kCanConcrete };
constexpr FieldDef kPlainDefs[] = {{L"height", false},     {L"height_interp", false}, {L"maxHeight", false},
                                   {L"soap", false},       {L"alcoholP", false},      {L"alcohol", true},
                                   {L"alcReady", true},    {L"empty", true},          {L"canConcrete", true}};
enum ConcreteF : size_t { kUnits, kDryTimer, kStage, kDry };
constexpr FieldDef kConcreteDefs[] = {{L"units", false}, {L"dryTimer", false}, {L"stage", false}, {L"dry", true}};
enum BagF : size_t { kBagUnits, kBagMax };
constexpr FieldDef kBagDefs[] = {{L"units", false}, {L"maxUnits", false}};
constexpr FieldDef kTrowelDefs[] = {{L"concrete", false}};
constexpr FieldDef kAlcDefs[] = {{L"itemsInside", false}};

template <size_t N>
struct Table {
    const FieldDef (&defs)[N];
    ObjKeyedCache<Layout, 4> cache;
    explicit Table(const FieldDef (&d)[N]) : defs(d) {}
};

Table<9> g_plain(kPlainDefs);
Table<4> g_concrete(kConcreteDefs);
Table<2> g_bag(kBagDefs);
Table<1> g_trowel(kTrowelDefs);
Table<1> g_alc(kAlcDefs);

// A TArray's header as UE4.27 lays it out: the element pointer, the length, the capacity.
struct ArrayHeader {
    void** data;
    int32_t num;
    int32_t max;
};

// The layout of `actor`'s class, found on its first actor and kept per class object (a Blueprint class dies with its world, and
// the cache reads a dead class as absent). A member that does not resolve is said once per class and the layout stays not-ok.
template <size_t N>
const Layout* LayoutOf(Table<N>& t, void* actor) {
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return nullptr;
    if (Layout* const hit = t.cache.Find(cls)) return hit->ok ? hit : nullptr;
    Layout l;
    l.ok = true;
    for (size_t i = 0; i < N; ++i) {
        Field& f = l.f[i];
        if (t.defs[i].isBool) {
            if (!R::FindBoolProperty(cls, t.defs[i].name, f.off, f.mask) || f.off < 0 || f.mask == 0) l.ok = false;
        } else {
            f.off = R::FindPropertyOffset(cls, t.defs[i].name);
            if (f.off < 0) l.ok = false;
        }
        if (!l.ok) {
            UE_LOGE("bucket: member '%ls' did not resolve on %ls -- its actors are neither read nor written", t.defs[i].name,
                    R::ClassNameOf(actor).c_str());
            break;
        }
    }
    const Layout& kept = t.cache.Insert(cls, l);
    return kept.ok ? &kept : nullptr;
}

template <class T>
T* At(void* actor, const Field& f) {
    return reinterpret_cast<T*>(static_cast<uint8_t*>(actor) + f.off);
}
bool BoolOf(void* actor, const Field& f) { return (*At<uint8_t>(actor, f) & f.mask) != 0; }
void SetBool(void* actor, const Field& f, bool v) {
    uint8_t& b = *At<uint8_t>(actor, f);
    b = v ? static_cast<uint8_t>(b | f.mask) : static_cast<uint8_t>(b & ~f.mask);
}

// ---- the kind of a class ----

constexpr const wchar_t* kKindClass[] = {L"prop_bucket_C", L"prop_concreteBucket_C", L"prop_cementBag_C", L"prop_wallfixer_C",
                                         L"prop_sponge_C"};
constexpr Kind kKindOfClass[] = {Kind::Plain, Kind::Concrete, Kind::Bag, Kind::Trowel, Kind::Sponge};
constexpr size_t kKinds = sizeof(kKindClass) / sizeof(kKindClass[0]);

ObjKeyedCache<Kind, 32> g_kinds;

// The five base classes by name. All are looked up again when the set of listed classes moved (a class listed grows the class log, an
// unload moves the unlist number); a base that answered null is asked again on every call until it answers, since a class is listed
// before it is readable and its finishing moves no number (object_index.h, ClassSetVersion).
struct Bases {
    void* cls[kKinds] = {};
    bool allLoaded = false;
    size_t logSize = 0;
    uint64_t unlisted = 0;
    bool resolved = false;
};
Bases g_bases;

const Bases& BasesNow() {
    const size_t logSize = object_index::ClassLogSize();
    const uint64_t unlisted = object_index::ClassUnlistVersion();
    const bool moved = !g_bases.resolved || g_bases.logSize != logSize || g_bases.unlisted != unlisted;
    if (!moved && g_bases.allLoaded) return g_bases;
    g_bases.allLoaded = true;
    for (size_t i = 0; i < kKinds; ++i) {
        if (moved || !g_bases.cls[i]) g_bases.cls[i] = object_index::ClassByName(kKindClass[i]);
        if (!g_bases.cls[i]) g_bases.allLoaded = false;
    }
    g_bases.logSize = logSize;
    g_bases.unlisted = unlisted;
    g_bases.resolved = true;
    return g_bases;
}

Kind ClassifyClass(void* cls, bool& decided) {
    const Bases& b = BasesNow();
    for (size_t i = 0; i < kKinds; ++i) {
        void* base = b.cls[i];
        if (base && R::IsDescendantOfAny(cls, &base, 1)) {
            decided = true;
            return kKindOfClass[i];
        }
    }
    // A base that is not loaded yet decides nothing: the class is asked again once it is.
    decided = b.allLoaded;
    return Kind::None;
}

bool CallNoArgs(void* actor, const wchar_t* fn) {
    void* const f = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), fn) : nullptr;
    ParamFrame frame(f);
    return f && frame.valid() && Call(actor, frame);
}

}  // namespace

Kind KindOfClass(void* cls) {
    if (!cls) return Kind::None;
    if (const Kind* const hit = g_kinds.Find(cls)) return *hit;
    bool decided = false;
    const Kind k = ClassifyClass(cls, decided);
    if (decided) g_kinds.Insert(cls, k);
    return k;
}

Kind KindOf(void* actor) { return actor ? KindOfClass(R::ClassOf(actor)) : Kind::None; }

bool ReadRecord(Kind kind, const save_record::SaveRecord& rec, Record& out) {
    Record r;
    switch (kind) {
    case Kind::Plain:
        if (rec.ints.empty() || rec.ints[0].empty() || rec.floats.size() < 2 || rec.floats[1].empty() || rec.bools.size() < 2 ||
            rec.bools[1].empty())
            return false;
        r.soap = rec.ints[0][0];
        r.height = rec.floats[1][0];
        r.alcohol = rec.bools[1][0] != 0;
        break;
    case Kind::Concrete:
        if (rec.ints.empty() || rec.ints[0].empty() || rec.bools.size() < 2 || rec.bools[1].empty()) return false;
        r.units = rec.ints[0][0];
        r.dry = rec.bools[1][0] != 0;
        break;
    case Kind::Bag:
        if (rec.ints.empty() || rec.ints[0].empty()) return false;
        r.units = rec.ints[0][0];
        break;
    case Kind::Trowel:
        if (rec.ints.empty() || rec.ints[0].empty()) return false;
        r.concrete = rec.ints[0][0];
        break;
    default:
        return false;
    }
    out = r;
    return true;
}

bool ReadPlain(void* bucket, Plain& out) {
    const Layout* const l = LayoutOf(g_plain, bucket);
    if (!l) return false;
    out.height = *At<float>(bucket, l->f[kHeight]);
    out.heightInterp = *At<float>(bucket, l->f[kInterp]);
    out.maxHeight = *At<float>(bucket, l->f[kMaxHeight]);
    out.soap = *At<int32_t>(bucket, l->f[kSoap]);
    out.alcoholP = *At<float>(bucket, l->f[kAlcoholP]);
    out.alcohol = BoolOf(bucket, l->f[kAlcohol]);
    out.alcReady = BoolOf(bucket, l->f[kAlcReady]);
    out.empty = BoolOf(bucket, l->f[kEmpty]);
    out.canConcrete = BoolOf(bucket, l->f[kCanConcrete]);
    return true;
}

bool WritePlain(void* bucket, float height, int32_t soap, float alcoholP, bool alcohol, bool alcReady) {
    const Layout* const l = LayoutOf(g_plain, bucket);
    if (!l) return false;
    *At<float>(bucket, l->f[kHeight]) = height;
    *At<int32_t>(bucket, l->f[kSoap]) = soap;
    *At<float>(bucket, l->f[kAlcoholP]) = alcoholP;
    SetBool(bucket, l->f[kAlcohol], alcohol);
    SetBool(bucket, l->f[kAlcReady], alcReady);
    SetBool(bucket, l->f[kEmpty], height <= 0.f);
    return true;
}

bool ReadConcrete(void* bucket, Concrete& out) {
    const Layout* const l = LayoutOf(g_concrete, bucket);
    if (!l) return false;
    out.units = *At<int32_t>(bucket, l->f[kUnits]);
    out.stage = *At<int32_t>(bucket, l->f[kStage]);
    out.dryTimer = *At<float>(bucket, l->f[kDryTimer]);
    out.dry = BoolOf(bucket, l->f[kDry]);
    return true;
}

bool WriteConcrete(void* bucket, int32_t units, float dryTimer, bool dry) {
    const Layout* const l = LayoutOf(g_concrete, bucket);
    if (!l) return false;
    *At<int32_t>(bucket, l->f[kUnits]) = units;
    *At<float>(bucket, l->f[kDryTimer]) = dryTimer;
    SetBool(bucket, l->f[kDry], dry);
    return true;
}

bool ReadBag(void* bag, int32_t& units, int32_t& maxUnits) {
    const Layout* const l = LayoutOf(g_bag, bag);
    if (!l) return false;
    units = *At<int32_t>(bag, l->f[kBagUnits]);
    maxUnits = *At<int32_t>(bag, l->f[kBagMax]);
    return true;
}

bool WriteBagUnits(void* bag, int32_t units) {
    const Layout* const l = LayoutOf(g_bag, bag);
    if (!l) return false;
    *At<int32_t>(bag, l->f[kBagUnits]) = units;
    return true;
}

bool ReadTrowel(void* trowel, int32_t& concrete) {
    const Layout* const l = LayoutOf(g_trowel, trowel);
    if (!l) return false;
    concrete = *At<int32_t>(trowel, l->f[0]);
    return true;
}

bool WriteTrowel(void* trowel, int32_t concrete) {
    const Layout* const l = LayoutOf(g_trowel, trowel);
    if (!l) return false;
    *At<int32_t>(trowel, l->f[0]) = concrete;
    return true;
}

bool ReadItemsInside(void* bucket, void** out, size_t max, size_t& total) {
    const Layout* const l = KindOf(bucket) == Kind::Plain ? LayoutOf(g_alc, bucket) : nullptr;
    if (!l) return false;
    const ArrayHeader a = *At<ArrayHeader>(bucket, l->f[0]);
    total = a.num > 0 && a.data ? static_cast<size_t>(a.num) : 0;
    for (size_t i = 0; i < total && i < max; ++i) out[i] = a.data[i];
    return true;
}

bool CallUpd(void* bucket) { return KindOf(bucket) == Kind::Plain && CallNoArgs(bucket, L"upd"); }
bool CallDipEffect(void* bucket) { return KindOf(bucket) == Kind::Plain && CallNoArgs(bucket, L"dipEffect"); }
bool CallUpdStage(void* bucket) { return KindOf(bucket) == Kind::Concrete && CallNoArgs(bucket, L"updStage"); }
bool CallUpdDry(void* bucket) { return KindOf(bucket) == Kind::Concrete && CallNoArgs(bucket, L"updDry"); }
bool CallUpdConcrete(void* trowel) { return KindOf(trowel) == Kind::Trowel && CallNoArgs(trowel, L"updConcrete"); }
bool CallDeleteAlc(void* bucket) { return KindOf(bucket) == Kind::Plain && CallNoArgs(bucket, L"deleteAlc"); }

bool CallTakeConcrete(void* bucket, bool& dryOut) {
    if (KindOf(bucket) != Kind::Concrete) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(bucket), L"takeConcrete"));
    if (!f.valid() || !Call(bucket, f)) return false;
    dryOut = f.Get<bool>(L"dry");
    return true;
}

bool CallDipped(void* bucket, void* sponge, bool& isDipped) {
    if (KindOf(bucket) != Kind::Plain || !sponge) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(bucket), L"dipped"));
    if (!f.valid() || !f.Set<void*>(L"sponge", sponge) || !Call(bucket, f)) return false;
    isDipped = f.Get<bool>(L"isDipped");
    return true;
}

bool CallHandUseLmb(void* item, void* player) {
    const Kind k = KindOf(item);
    if (!player || (k != Kind::Sponge && k != Kind::Bag && k != Kind::Trowel)) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(item), L"playerHandUse_LMB"));
    return f.valid() && f.Set<void*>(L"player", player) && Call(item, f);
}

void* ReplaceProp(void* prop, const wchar_t* newName, void* worldContext) {
    void* const lib = R::FindClassDefaultObject(L"lib_C");
    void* const fn = lib ? R::FindDispatchFunctionCached(R::ClassOf(lib), L"replaceProp") : nullptr;
    if (!prop || !newName || !worldContext || !fn) return nullptr;
    const R::FName name = fname_utils::StringToFName(newName);
    if (name.ComparisonIndex == 0) return nullptr;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"prop", prop) || !f.SetRaw(L"new", &name, sizeof(name)) ||
        !f.Set<bool>(L"delete", false) || !f.Set<void*>(L"playerHolding", nullptr) ||
        !f.Set<void*>(L"__WorldContext", worldContext) || !Call(lib, f))
        return nullptr;
    return f.Get<void*>(L"return");
}

}  // namespace ue_wrap::bucket
