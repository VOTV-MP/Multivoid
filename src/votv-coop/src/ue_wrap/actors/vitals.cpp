// ue_wrap/actors/vitals.cpp -- see ue_wrap/actors/vitals.h.

#include "ue_wrap/actors/vitals.h"

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

namespace ue_wrap::vitals {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// One-time resolution cache. Resolving on every access ran several FindPropertyOffset calls --
// ~100-300 ms of game-thread block each time, a visible frame hitch. Cached, the steady-state cost
// is one pointer deref plus one float access; the GameInstance is the world singleton's.
struct Cache {
    int32_t saveGameInstOff = -1;        // mainGameInstance_C::save_gameInst (UsaveSlot_C*)
    void* saveSlotClass = nullptr;       // UClass* for UsaveSlot_C (offset-lookup target)
    int32_t fieldOff[4] = {-1, -1, -1, -1};  // indexed by Field
};
Cache g_cache;

const wchar_t* FieldName(Field f) {
    switch (f) {
        case Field::Health:    return L"health";
        case Field::MaxHealth: return L"maxHealth";
        case Field::Food:      return L"food";
        case Field::Sleep:     return L"sleep";
    }
    return L"";
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

int32_t ResolveFieldOffset(Field f) {
    const int idx = static_cast<int>(f);
    if (g_cache.fieldOff[idx] < 0 && g_cache.saveSlotClass) {
        g_cache.fieldOff[idx] = R::FindPropertyOffset(g_cache.saveSlotClass, FieldName(f));
    }
    return g_cache.fieldOff[idx];
}

// ---- the snapshot: every per-player field of the save object, resolved by name once ----------

namespace SR = ue_wrap::save_record;

struct ScalarRow { const wchar_t* name; float Snapshot::* member; };
constexpr ScalarRow kScalars[] = {
    {L"health", &Snapshot::health},           {L"maxHealth", &Snapshot::maxHealth},
    {L"food", &Snapshot::food},               {L"sleep", &Snapshot::sleep},
    {L"battery", &Snapshot::battery},         {L"coffeePower", &Snapshot::coffeePower},
    {L"gasolinepilled", &Snapshot::gasolinepilled},
    {L"strength", &Snapshot::strength},       {L"agility", &Snapshot::agility},
};
constexpr size_t kScalarCount = sizeof(kScalars) / sizeof(kScalars[0]);

struct SnapshotOffsets {
    bool    looked = false, ok = false;
    int32_t scalar[kScalarCount] = {};
    int32_t flashlightBattery = -1, foodConsumed = -1, foodTolerance = -1;
};
SnapshotOffsets g_snap;

// Never guessed: one unresolvable name makes the whole snapshot inert, since a half-written set of
// vitals is worse than the host's.
bool EnsureSnapshotOffsets() {
    if (g_snap.looked) return g_snap.ok;
    if (!EnsureBase()) return false;  // not latched: the class may simply not be loaded yet
    g_snap.looked = true;
    bool ok = true;
    auto find = [&ok](const wchar_t* name) {
        const int32_t off = R::FindPropertyOffset(g_cache.saveSlotClass, name);
        if (off < 0) {
            ok = false;
            UE_LOGE("vitals: saveSlot.%ls did not resolve -- the vitals snapshot is inert", name);
        }
        return off;
    };
    for (size_t i = 0; i < kScalarCount; ++i) g_snap.scalar[i] = find(kScalars[i].name);
    g_snap.flashlightBattery = find(L"flashlightBattery");
    g_snap.foodConsumed      = find(L"food_consumed");
    g_snap.foodTolerance     = find(L"food_tolerance");
    g_snap.ok = ok;
    return ok;
}

std::wstring ReadFString(const uint8_t* e) {
    R::FString f{};
    std::memcpy(&f, e, sizeof(f));
    if (!SR::PlausibleObjPtr(f.Data) || f.Num <= 1 || f.Num > 4096) return {};
    return std::wstring(f.Data, static_cast<size_t>(f.Num - 1));  // Num counts the terminator
}

void ReadFrom(const void* slot, Snapshot& out) {
    const auto* base = static_cast<const uint8_t*>(slot);
    for (size_t i = 0; i < kScalarCount; ++i)
        std::memcpy(&(out.*kScalars[i].member), base + g_snap.scalar[i], sizeof(float));
    void* cls = nullptr;
    std::memcpy(&cls, base + g_snap.flashlightBattery, sizeof(cls));
    out.flashlightBattery =
        (cls && SR::PlausibleObjPtr(cls) && R::IsLive(cls)) ? R::ToString(R::NameOf(cls)) : std::wstring{};
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

}  // namespace

bool ReadSnapshot(Snapshot& out) {
    void* slot = ResolveSlot();
    if (!slot || !R::IsLive(slot) || !EnsureSnapshotOffsets()) return false;
    ReadFrom(slot, out);
    return true;
}

bool ReadDefaults(Snapshot& out) {
    // Found by a walk of the object array, so read once: a class's defaults do not change.
    static Snapshot s_defaults;
    static bool s_have = false;
    if (!s_have) {
        if (!EnsureSnapshotOffsets()) return false;
        void* cdo = R::FindClassDefaultObject(P::name::SaveSlotClass);
        if (!cdo) return false;
        ReadFrom(cdo, s_defaults);
        s_have = true;
    }
    out = s_defaults;
    return true;
}

bool ApplySnapshot(void* saveSlot, const Snapshot& s) {
    if (!saveSlot || !R::IsLive(saveSlot) || !EnsureSnapshotOffsets()) return false;
    auto* base = static_cast<uint8_t*>(saveSlot);
    for (size_t i = 0; i < kScalarCount; ++i)
        std::memcpy(base + g_snap.scalar[i], &(s.*kScalars[i].member), sizeof(float));
    void* battery = s.flashlightBattery.empty() ? nullptr : R::FindClass(s.flashlightBattery.c_str());
    if (battery || s.flashlightBattery.empty())  // an empty name IS the ejected battery
        std::memcpy(base + g_snap.flashlightBattery, &battery, sizeof(battery));
    else
        UE_LOGW("vitals: battery class '%ls' is not loaded -- the field keeps what it had",
                s.flashlightBattery.c_str());

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

bool Read(Field f, float* out) {
    void* slot = ResolveSlot();
    if (!slot) return false;
    const int32_t off = ResolveFieldOffset(f);
    if (off < 0) return false;
    if (out) *out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + off);
    return true;
}

bool Write(Field f, float v) {
    void* slot = ResolveSlot();
    if (!slot) return false;
    const int32_t off = ResolveFieldOffset(f);
    if (off < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + off) = v;
    return true;
}

}  // namespace ue_wrap::vitals
