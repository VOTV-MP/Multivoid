// ue_wrap/actors/vitals.cpp -- see ue_wrap/actors/vitals.h.

#include "ue_wrap/actors/vitals.h"

#include "ue_wrap/actors/player_record.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap::vitals {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// One-time resolution cache. Resolving on every access ran several FindPropertyOffset calls --
// ~100-300 ms of game-thread block each time, a visible frame hitch. Cached, no access looks a
// property up again; the steady-state cost differs per owner. A save row: the world singleton's
// GameInstance lookup twice (EnsureBase, then ResolveSlot), a class compare (EnsureSaveRows), then
// the save_gameInst deref and the float access. A gamemode row: the gamemode lookup
// and a class compare first. A pawn row: that, then the gamemode's mainPlayer deref, an IsLive
// check and a class compare.
struct Cache {
    int32_t saveGameInstOff = -1;        // mainGameInstance_C::save_gameInst (UsaveSlot_C*)
    void* saveSlotClass = nullptr;       // UClass* for UsaveSlot_C (offset-lookup target)
};
Cache g_cache;

// ---- the stat table: each row on its own owner, its property named here and nowhere else -------

using F = Field;
constexpr Row kRows[] = {
    {F::Health,         "health",      Owner::Pawn,     L"save_health",         Type::Float, WriteRule::Raw},
    {F::MaxHealth,      "maxhealth",   Owner::Pawn,     L"save_maxhealth",      Type::Float, WriteRule::Raw},
    {F::Food,           "food",        Owner::Pawn,     L"save_food",           Type::Float, WriteRule::Raw},
    {F::Sleep,          "sleep",       Owner::Pawn,     L"save_sleep",          Type::Float, WriteRule::Raw},
    {F::CoffeePower,    "coffee",      Owner::Pawn,     L"save_coffeepower",    Type::Float, WriteRule::Raw},
    {F::GasolinePilled, "gasoline",    Owner::Pawn,     L"save_gasolinepilled", Type::Float, WriteRule::Raw},
    {F::Strength,       "strength",    Owner::SaveSlot, L"strength",            Type::Float, WriteRule::Raw},
    {F::Irradiation,    "radiation",   Owner::Pawn,     L"irradiation",         Type::Float, WriteRule::Raw},
    {F::Air,            "air",         Owner::Pawn,     L"air",                 Type::Float, WriteRule::Raw},
    {F::BurningTime,    "burntime",    Owner::Pawn,     L"burningTime",         Type::Float, WriteRule::Raw},
    {F::Pooped,         "pooped",      Owner::Pawn,     L"pooped",              Type::Float, WriteRule::Raw},
    {F::FoodDrain,      "fooddrain",   Owner::Pawn,     L"foodDraining",        Type::Float, WriteRule::Raw},
    {F::SleepDrain,     "sleepdrain",  Owner::Pawn,     L"sleepDraining",       Type::Float, WriteRule::Raw},
    {F::Burning,        "burning",     Owner::Pawn,     L"isBurning",           Type::Bool,  WriteRule::ReadOnly},
    {F::Dead,           "dead",        Owner::Pawn,     L"dead",                Type::Bool,  WriteRule::ReadOnly},
    {F::Sleeping,       "sleeping",    Owner::Gamemode, L"isSleep",             Type::Bool,  WriteRule::ReadOnly},
    {F::Dreaming,       "dreaming",    Owner::Gamemode, L"dreaming",            Type::Bool,  WriteRule::ReadOnly},
    {F::Exhausted,      "exhausted",   Owner::Pawn,     L"isExhausted_bool",    Type::Bool,  WriteRule::ReadOnly},
    {F::Nearsighted,    "nearsighted", Owner::Pawn,     L"velmaMode",           Type::Bool,  WriteRule::ReadOnly},
    {F::Glasses,        "glasses",     Owner::Pawn,     L"equipped_glasses",    Type::Bool,  WriteRule::ReadOnly},
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
constexpr size_t kPawnSnapshotRows = 6;  // rows 0-5: the pawn's save_* floats
constexpr size_t kSnapshotRows = 7;      // ... and row 6, the save object's Strength

constexpr bool RowsInOrder() {
    for (size_t i = 0; i < kRowCount; ++i)
        if (static_cast<size_t>(kRows[i].field) != i) return false;
    return true;
}
static_assert(kRowCount == static_cast<size_t>(Field::Count), "one row per Field value");
static_assert(RowsInOrder(), "kRows[i].field == i: a reordered row does not compile");

// The snapshot's rows are the pawn's saved floats, then the save object's Strength: the pawn's rows are
// written into the player's record by their property name, which is the name the record stores them under.
constexpr bool SnapshotRowsShape() {
    for (size_t i = 0; i < kSnapshotRows; ++i) {
        const Owner want = i < kPawnSnapshotRows ? Owner::Pawn : Owner::SaveSlot;
        if (kRows[i].owner != want || kRows[i].type != Type::Float) return false;
    }
    return kRows[kSnapshotRows - 1].field == Field::Strength;
}
static_assert(kSnapshotRows <= kRowCount && SnapshotRowsShape(),
              "rows 0-5 are Pawn floats, row 6 is the SaveSlot float Strength");

// Where each row's property sits on its owner's class. mask 0 is a Float; a Bool carries its byte
// and bit mask. -1: it did not resolve for the class the owner's stamp names.
struct Resolved { int32_t off = -1; uint8_t mask = 0; };
Resolved g_res[kRowCount];

void* g_saveRowsCls = nullptr;   // the class g_res's SaveSlot rows were resolved for
void* g_gamemodeCls = nullptr;   // ... the Gamemode rows
void* g_pawnCls = nullptr;       // ... the Pawn rows
int32_t g_gamemodeMainPlayerOff = -1;  // mainGamemode_C.mainPlayer: the pawn, off the gamemode

// Resolve every row of `owner` on `cls` at once. A name that does not resolve stays unresolved for
// this class and is logged once.
void ResolveRows(Owner owner, void* cls) {
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (r.owner != owner) continue;
        Resolved& res = g_res[i];
        res = Resolved{};
        if (r.type == Type::Float) {
            res.off = R::FindPropertyOffset(cls, r.property);
        } else {
            int32_t byte = -1;
            uint8_t mask = 0;
            if (R::FindBoolProperty(cls, r.property, byte, mask) && mask != 0) { res.off = byte; res.mask = mask; }
        }
        if (res.off < 0)
            UE_LOGE("vitals: %ls.%ls did not resolve", R::ToString(R::NameOf(cls)).c_str(), r.property);
    }
}

// Resolve GameInstance + the save_gameInst offset + the saveSlot UClass. Returns
// false if any step isn't up yet. Game-thread only.
bool EnsureBase() {
    void* gi = world_singleton::GameInstance();
    if (!gi) return false;
    if (g_cache.saveGameInstOff < 0) {
        void* giClass = R::ClassOf(gi);
        if (!giClass) return false;
        g_cache.saveGameInstOff = R::FindPropertyOffset(giClass, L"save_gameInst");
        if (g_cache.saveGameInstOff < 0) return false;
    }
    if (!g_cache.saveSlotClass) {
        g_cache.saveSlotClass = R::FindClass(P::name::SaveSlotClass);
        if (!g_cache.saveSlotClass) return false;
    }
    return true;
}

// The canonical live saveSlot pointer (mainGameInstance.save_gameInst). null if
// the save isn't registered yet. Pointing THROUGH the GameInstance (rather than
// FindObjectByClass(saveSlot_C), which walks GUObjectArray and can surface a
// stale menu-era UsaveSlot_C from ui_saveSlots arrays) is the unambiguous path.
void* ResolveSlot() {
    if (!EnsureBase()) return nullptr;
    void* gi = world_singleton::GameInstance();
    return gi ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gi) + g_cache.saveGameInstOff) : nullptr;
}

// The save owner's rows, resolved against the save class once it is found by name.
void EnsureSaveRows() {
    if (g_saveRowsCls == g_cache.saveSlotClass) return;
    g_saveRowsCls = g_cache.saveSlotClass;
    ResolveRows(Owner::SaveSlot, g_saveRowsCls);
}

// The running world's gamemode with its rows (and its `mainPlayer`) resolved for its class, the
// container_view.cpp shape. Null when there is none.
void* GamemodeObject() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (void* const cls = R::ClassOf(gm); cls != g_gamemodeCls) {
        g_gamemodeCls = cls;
        ResolveRows(Owner::Gamemode, cls);
        g_gamemodeMainPlayerOff = R::FindPropertyOffset(cls, L"mainPlayer");
        if (g_gamemodeMainPlayerOff < 0)
            UE_LOGE("vitals: %ls.mainPlayer did not resolve", R::ToString(R::NameOf(cls)).c_str());
    }
    return gm;
}

// The pawn rows resolved for `cls`, the class of the pawn or of its class default object.
void EnsurePawnRows(void* cls) {
    if (cls == g_pawnCls) return;
    g_pawnCls = cls;
    ResolveRows(Owner::Pawn, cls);
}

// The local player's pawn: the gamemode's own `mainPlayer`, live, with the pawn rows resolved for
// its class. Null when there is none.
void* PawnObject() {
    void* const gm = GamemodeObject();
    if (!gm || g_gamemodeMainPlayerOff < 0) return nullptr;
    void* const pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + g_gamemodeMainPlayerOff);
    if (!pawn || !R::IsLive(pawn)) return nullptr;
    EnsurePawnRows(R::ClassOf(pawn));
    return pawn;
}

// The object that owns `owner`'s rows, ready to read at g_res's offsets. The save object is not
// IsLive-checked (as it always was); the snapshot functions check it themselves.
void* OwnerObject(Owner owner) {
    switch (owner) {
        case Owner::SaveSlot: {
            void* const slot = ResolveSlot();
            if (slot) EnsureSaveRows();
            return slot;
        }
        case Owner::Gamemode: return GamemodeObject();
        case Owner::Pawn:     return PawnObject();
    }
    return nullptr;
}

// ---- the snapshot: the pawn's saved floats and the save object's per-player fields --------------

namespace SR = ue_wrap::save_record;

struct SnapshotOffsets {
    bool    looked = false, ok = false;
    int32_t foodConsumed = -1, foodTolerance = -1;
};
SnapshotOffsets g_snap;

// The save object's part, never guessed: one unresolvable name makes the whole snapshot inert, since
// a half-written set of vitals is worse than the host's.
bool EnsureSaveSnapshotOffsets() {
    if (g_snap.looked) return g_snap.ok;
    if (!EnsureBase()) return false;  // not latched: the class may simply not be loaded yet
    g_snap.looked = true;
    EnsureSaveRows();  // Strength resolves with the save class, and logs its own line
    bool ok = true;
    auto find = [&ok](const wchar_t* name) {
        const int32_t off = R::FindPropertyOffset(g_cache.saveSlotClass, name);
        if (off < 0) {
            ok = false;
            UE_LOGE("vitals: saveSlot.%ls did not resolve -- the vitals snapshot is inert", name);
        }
        return off;
    };
    for (size_t i = kPawnSnapshotRows; i < kSnapshotRows; ++i) {
        if (g_res[i].off >= 0) continue;
        ok = false;
        UE_LOGE("vitals: saveSlot.%ls did not resolve -- the vitals snapshot is inert", kRows[i].property);
    }
    g_snap.foodConsumed  = find(L"food_consumed");
    g_snap.foodTolerance = find(L"food_tolerance");
    g_snap.ok = ok;
    return ok;
}

// The pawn's part, resolved for `cls` (the pawn's class). False, said once per class, when a row does not.
bool PawnSnapshotRowsResolved(void* cls) {
    EnsurePawnRows(cls);
    for (size_t i = 0; i < kPawnSnapshotRows; ++i) {
        if (g_res[i].off >= 0) continue;
        static void* s_said = nullptr;
        if (s_said != cls) {
            s_said = cls;
            UE_LOGE("vitals: %ls.%ls did not resolve -- the vitals snapshot is inert",
                    R::ToString(R::NameOf(cls)).c_str(), kRows[i].property);
        }
        return false;
    }
    return true;
}

std::wstring ReadFString(const uint8_t* e) {
    R::FString f{};
    std::memcpy(&f, e, sizeof(f));
    if (!SR::PlausibleObjPtr(f.Data) || f.Num <= 1 || f.Num > 4096) return {};
    return std::wstring(f.Data, static_cast<size_t>(f.Num - 1));  // Num counts the terminator
}

void ReadPawnPart(const void* pawn, Snapshot& out) {
    const auto* base = static_cast<const uint8_t*>(pawn);
    for (size_t i = 0; i < kPawnSnapshotRows; ++i)
        std::memcpy(SnapshotField(out, static_cast<Field>(i)), base + g_res[i].off, sizeof(float));
}

void ReadSavePart(const void* slot, Snapshot& out) {
    const auto* base = static_cast<const uint8_t*>(slot);
    for (size_t i = kPawnSnapshotRows; i < kSnapshotRows; ++i)
        std::memcpy(SnapshotField(out, static_cast<Field>(i)), base + g_res[i].off, sizeof(float));
    const SR::Arr eaten = SR::ReadArr(slot, g_snap.foodConsumed);
    const SR::Arr tol   = SR::ReadArr(slot, g_snap.foodTolerance);
    const int32_t n = std::min(eaten.num, tol.num);  // parallel by construction; read them as such
    out.foodConsumed.clear();
    out.foodTolerance.clear();
    for (int32_t i = 0; i < n; ++i) {
        out.foodConsumed.push_back(ReadFString(eaten.data + static_cast<size_t>(i) * sizeof(R::FString)));
        float t = 0;
        std::memcpy(&t, tol.data + static_cast<size_t>(i) * sizeof(float), sizeof(float));
        out.foodTolerance.push_back(t);
    }
}

// A float as text a property import reads back to the same value.
std::wstring FloatText(float v) {
    wchar_t buf[48];
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.9g", static_cast<double>(v));
    return buf;
}

}  // namespace

bool SnapshotSourcesUp() {
    void* const slot = ResolveSlot();
    return PawnObject() && slot && R::IsLive(slot);
}

bool ReadSnapshot(Snapshot& out) {
    void* const pawn = PawnObject();
    void* const slot = ResolveSlot();
    if (!pawn || !slot || !R::IsLive(slot) || !EnsureSaveSnapshotOffsets()) return false;
    if (!PawnSnapshotRowsResolved(R::ClassOf(pawn))) return false;
    ReadPawnPart(pawn, out);
    ReadSavePart(slot, out);
    return true;
}

bool ReadDefaults(Snapshot& out, void* save) {
    // Found by a walk of the object array, so read once: a class's defaults do not change.
    static Snapshot s_defaults;
    static bool s_have = false;
    if (!s_have) {
        if (!EnsureSaveSnapshotOffsets()) return false;
        void* pawnCls = R::FindClass(P::name::MainPlayerClass);
        if (!pawnCls) pawnCls = ue_wrap::player_record::PlayerClass(save);
        if (!pawnCls || !PawnSnapshotRowsResolved(pawnCls)) return false;
        void* const pawnCdo = R::FindClassDefaultObject(P::name::MainPlayerClass);
        void* const saveCdo = R::FindClassDefaultObject(P::name::SaveSlotClass);
        if (!pawnCdo || !saveCdo) return false;
        ReadPawnPart(pawnCdo, s_defaults);
        ReadSavePart(saveCdo, s_defaults);
        s_have = true;
    }
    out = s_defaults;
    return true;
}

bool ApplySnapshot(void* saveSlot, const Snapshot& s) {
    if (!saveSlot || !R::IsLive(saveSlot) || !EnsureSaveSnapshotOffsets()) return false;
    for (size_t i = 0; i < kPawnSnapshotRows; ++i)
        if (!std::isfinite(*SnapshotField(s, static_cast<Field>(i)))) return false;
    // The pawn's rows first, into the record the level's pawn loads them from: the record stores each under the
    // property's own name. A save object with no record leaves the pawn at its class's own values.
    std::vector<std::pair<std::wstring, std::wstring>> props;
    for (size_t i = 0; i < kPawnSnapshotRows; ++i)
        props.emplace_back(kRows[i].property, FloatText(*SnapshotField(s, static_cast<Field>(i))));
    if (ue_wrap::player_record::CountInSave(saveSlot) > 0 &&
        ue_wrap::player_record::SetPropertiesInSave(saveSlot, props) <= 0) {
        UE_LOGW("vitals: the player's record did not take the vitals");
        return false;
    }
    auto* base = static_cast<uint8_t*>(saveSlot);
    for (size_t i = kPawnSnapshotRows; i < kSnapshotRows; ++i)
        std::memcpy(base + g_res[i].off, SnapshotField(s, static_cast<Field>(i)), sizeof(float));

    const size_t n = std::min(s.foodConsumed.size(), s.foodTolerance.size());
    void* eaten = SR::AllocZeroed(n, sizeof(R::FString));
    void* tol   = SR::AllocZeroed(n, sizeof(float));
    if (n && (!eaten || !tol)) {
        UE_LOGW("vitals: engine alloc failed -- the food tolerance arrays keep what they had");
        return true;  // the scalars are written; the arrays are still a consistent pair
    }
    for (size_t i = 0; i < n; ++i) {
        ue_wrap::fstring_utils::MintFString(s.foodConsumed[i], static_cast<uint8_t*>(eaten) + i * sizeof(R::FString));
        std::memcpy(static_cast<uint8_t*>(tol) + i * sizeof(float), &s.foodTolerance[i], sizeof(float));
    }
    SR::WriteArrHeader(saveSlot, g_snap.foodConsumed, eaten, static_cast<int32_t>(n));
    SR::WriteArrHeader(saveSlot, g_snap.foodTolerance, tol, static_cast<int32_t>(n));
    return true;
}

bool WritePlayerTransform(void* saveSlot, float x, float y, float z, float yawDeg) {
    if (!saveSlot || !R::IsLive(saveSlot) || !EnsureBase()) return false;
    static int32_t s_off = -2;
    if (s_off == -2) s_off = R::FindPropertyOffset(g_cache.saveSlotClass, L"playerTransform");
    if (s_off < 0) return false;
    const float half = yawDeg * 0.5f * 3.14159265358979f / 180.f;
    ue_wrap::FTransform t{};
    t.RotX = 0.f; t.RotY = 0.f; t.RotZ = std::sin(half); t.RotW = std::cos(half);  // yaw about Z
    t.TX = x; t.TY = y; t.TZ = z;
    t.SX = t.SY = t.SZ = 1.f;
    std::memcpy(static_cast<uint8_t*>(saveSlot) + s_off, &t, sizeof(t));
    return true;
}

const Row& RowOf(Field f) { return kRows[static_cast<size_t>(f)]; }

bool FieldFromId(uint8_t id, Field* out) {
    if (id >= static_cast<uint8_t>(Field::Count)) return false;
    if (out) *out = static_cast<Field>(id);
    return true;
}

float* SnapshotField(Snapshot& s, Field f) {
    switch (f) {
        case Field::Health:         return &s.health;
        case Field::MaxHealth:      return &s.maxHealth;
        case Field::Food:           return &s.food;
        case Field::Sleep:          return &s.sleep;
        case Field::CoffeePower:    return &s.coffeePower;
        case Field::GasolinePilled: return &s.gasolinepilled;
        case Field::Strength:       return &s.strength;
        default:                    return nullptr;
    }
}

const float* SnapshotField(const Snapshot& s, Field f) { return SnapshotField(const_cast<Snapshot&>(s), f); }

bool Read(Field f, float* out) {
    if (f >= Field::Count) return false;
    const size_t idx = static_cast<size_t>(f);
    const void* const obj = OwnerObject(kRows[idx].owner);
    if (!obj) return false;
    const Resolved& res = g_res[idx];
    if (res.off < 0) return false;
    const uint8_t* const p = static_cast<const uint8_t*>(obj) + res.off;
    float v = 0.f;
    if (kRows[idx].type == Type::Float) std::memcpy(&v, p, sizeof(v));
    else v = (*p & res.mask) != 0 ? 1.f : 0.f;
    if (out) *out = v;
    return true;
}

bool Write(Field f, float v) {
    if (f >= Field::Count) return false;
    const size_t idx = static_cast<size_t>(f);
    const Row& row = kRows[idx];
    if (row.write == WriteRule::ReadOnly || !std::isfinite(v)) return false;
    void* const obj = OwnerObject(row.owner);
    if (!obj) return false;
    const Resolved& res = g_res[idx];
    if (res.off < 0) return false;
    uint8_t* const p = static_cast<uint8_t*>(obj) + res.off;
    if (row.type == Type::Float) {
        std::memcpy(p, &v, sizeof(v));
    } else if (v != 0.f) {
        *p |= res.mask;
    } else {
        *p &= static_cast<uint8_t>(~res.mask);
    }
    return true;
}

}  // namespace ue_wrap::vitals
