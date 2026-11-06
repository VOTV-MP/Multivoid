// ue_wrap/devices/atv.cpp -- see ue_wrap/devices/atv.h. Engine access for the ATV/quadbike
// (AATV_C). Offsets are resolved from the live class through reflection, with the Alpha
// 0.9.0-n values kept as logged fallbacks. Transform reads and writes go through
// ue_wrap::engine at the actor level: the root Mesh IS the actor root, so the actor transform
// is the physics body transform.

#include "ue_wrap/devices/atv.h"

#include "ue_wrap/actors/kerfur.h"   // the kerfur's own ATV functions (InviteKerfurs, ReleaseKerfurs)
#include "ue_wrap/actors/save_record.h"  // allKerfuros, a new array in engine memory
#include "ue_wrap/core/call.h"      // ParamFrame / Call -- the GameplayStatics spawn (SpawnMirror)
#include "ue_wrap/core/fname_utils.h"  // StringToFName -- SpawnMirror's key
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"     // FTransform (SpawnMirror)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ue_wrap::atv {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};
int32_t g_keyOff      = -1;       // Key      (Alpha 0.9.0-n: 0x0618, FName)
int32_t g_playerOff   = -1;       // Player   (0x05B0, AmainPlayer_C*)
int32_t g_isDrivenOff = -1;       // isDriven (0x05F7, bool)
int32_t g_fuelOff     = -1;       // fuel     (0x05D4, float)
int32_t g_healthOff   = -1;       // health   (0x05E4, float)
int32_t g_brakeOff    = -1;       // Brake    (0x05D9, bool)
int32_t g_trapOff     = -1;       // trap     (a bool bit: the byte, with its mask below)
uint8_t g_trapMask    = 0;
int32_t g_tpOff       = -1;       // tp       (a UBillboardComponent*: where `explode` places its blast)

int32_t ResolveOff(void* cls, const wchar_t* name, int32_t fallback) {
    int32_t o = R::FindPropertyOffset(cls, name);
    if (o < 0) {
        UE_LOGW("atv: %ls offset not found -- using fallback 0x%04X", name, fallback);
        o = fallback;
    }
    return o;
}

// ATV_C as the object index holds it, looked up where it is used -- one lookup that walks nothing --
// and never kept: a Blueprint class can come back as a new object after a world change (127 did in
// a same-process rejoin, ATV_C not among them), and one still loading answers null here. The field
// offsets above are its layout, the same in every world, so they are resolved once.
void* AtvClass() { return object_index::ClassByName(L"ATV_C"); }

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = AtvClass();
    if (!cls) return false;

    g_keyOff      = ResolveOff(cls, L"Key",      0x0618);
    g_playerOff   = ResolveOff(cls, L"Player",   0x05B0);
    g_isDrivenOff = ResolveOff(cls, L"isDriven", 0x05F7);
    g_fuelOff     = ResolveOff(cls, L"fuel",     0x05D4);
    g_healthOff   = ResolveOff(cls, L"health",   0x05E4);
    g_brakeOff    = ResolveOff(cls, L"Brake",    0x05D9);
    // No fallback: a miss leaves the trap host-only, as it was before it travelled. The flag is a bit packed with its
    // neighbours (inWater, floater), so its byte and mask are both resolved.
    if (!R::FindBoolProperty(cls, L"trap", g_trapOff, g_trapMask) || g_trapOff < 0 || g_trapMask == 0) {
        g_trapOff = -1;
        UE_LOGW("atv: trap offset not found -- the story event's trap stays host-only");
    }
    // No fallback: a miss leaves a client's ATV explosion unrequested, since the host has no place to put it.
    g_tpOff = R::FindPropertyOffset(cls, L"tp");
    if (g_tpOff < 0) UE_LOGW("atv: tp offset not found -- a client's ATV explosion cannot be requested");

    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("atv: resolved ATV_C=%p Key@0x%04X Player@0x%04X isDriven@0x%04X fuel@0x%04X "
            "health@0x%04X Brake@0x%04X",
            cls, g_keyOff, g_playerOff, g_isDrivenOff, g_fuelOff, g_healthOff, g_brakeOff);
    return true;
}

bool IsAtv(void* obj) {
    if (!obj) return false;
    void* atvCls = AtvClass();
    if (!atvCls) return false;
    void* cls = R::ClassOf(obj);
    return cls && R::IsDescendantOfAny(cls, &atvCls, 1);
}

size_t ForEachAtv(void (*fn)(void* ctx, void* actor, int32_t idx), void* ctx) {
    if (!fn) return 0;
    // The classes first: the instance walk does not run from inside the class walk.
    std::vector<void*> classes;
    object_index::ForEachClass([](void* c, void* cls, void* anyInstance) {
        if (IsAtv(anyInstance)) static_cast<std::vector<void*>*>(c)->push_back(cls);
    }, &classes);
    struct Walk { void (*fn)(void*, void*, int32_t); void* ctx; size_t loading; } walk{fn, ctx, 0};
    for (void* cls : classes) {
        object_index::ForEachInstance(cls, [](void* p, void* obj, int32_t idx) {
            Walk* w = static_cast<Walk*>(p);
            const auto flags = R::SlotFlags(idx);
            if (flags & R::slot_flags::Dying) return;   // leaving: not counted
            if (flags & R::slot_flags::NotYetReadable) { ++w->loading; return; }
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            w->fn(w->ctx, obj, idx);
        }, &walk);
    }
    return walk.loading;
}

std::wstring GetKeyString(void* atv) {
    if (!atv || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(atv) + g_keyOff);
    return R::ToString(key);
}

bool GetRootTransform(void* atv, FVector& loc, FRotator& rot) {
    return atv && engine::TryGetActorLocation(atv, loc) && engine::TryGetActorRotation(atv, rot);
}

void* GetOccupantPlayer(void* atv) {
    if (!atv || g_playerOff < 0) return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(atv) + g_playerOff);
}

bool IsDriven(void* atv) {
    if (!atv || g_isDrivenOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(atv) + g_isDrivenOff);
}

float GetFuel(void* atv) {
    if (!atv || g_fuelOff < 0) return 0.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(atv) + g_fuelOff);
}

float GetHealth(void* atv) {
    if (!atv || g_healthOff < 0) return 0.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(atv) + g_healthOff);
}

bool GetBrake(void* atv) {
    if (!atv || g_brakeOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(atv) + g_brakeOff);
}

bool ReadTrap(void* atv, bool& out) {
    if (!atv || g_trapOff < 0) return false;
    out = (*(reinterpret_cast<const uint8_t*>(atv) + g_trapOff) & g_trapMask) != 0;
    return true;
}

bool WriteTrap(void* atv, bool v) {
    if (!atv || g_trapOff < 0) return false;
    uint8_t& byte = *(reinterpret_cast<uint8_t*>(atv) + g_trapOff);
    byte = v ? static_cast<uint8_t>(byte | g_trapMask) : static_cast<uint8_t>(byte & ~g_trapMask);
    return true;
}

bool ReadTpLocation(void* atv, FVector& out) {
    out = FVector{};
    if (!atv || g_tpOff < 0) return false;
    void* tp = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(atv) + g_tpOff);
    return tp && engine::TryGetComponentLocation(tp, out);
}

bool TeleportRig(void* atv, const FVector& loc, const FRotator& rot) {
    if (!atv || !EnsureResolved()) return false;
    // The memoised lookup, on the rig's own class: a warp comes at up to 20 Hz and a hit renders no
    // name, and the answer is held by slot and serial, so a class a world change replaced is asked
    // again and a miss is held as a miss while its class lives.
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(atv), L"teleportVehicle");
    if (!fn) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            UE_LOGE("atv: teleportVehicle unresolved -- the rig can never be warped; "
                    "a mirror that drifts past the threshold will not be re-placed");
        }
        return false;
    }
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<FVector>(L"NewLocation", loc);
    f.Set<FRotator>(L"NewRotation", rot);
    return Call(atv, f);
}

const char* SpawnFailText(SpawnFail why) {
    switch (why) {
        case SpawnFail::None:          return "no failure";
        case SpawnFail::Unresolved:    return "the ATV wrapper, the spawn functions or the world are unresolved";
        case SpawnFail::NoKey:         return "the class name or the key is empty";
        case SpawnFail::ClassNotFound: return "the class is not loaded";
        case SpawnFail::NotAnAtv:      return "the class is not an ATV subclass";
        case SpawnFail::NoOffset:      return "the class has no key property";
        case SpawnFail::NameNone:      return "the key did not convert to a name";
        case SpawnFail::BeginFailed:   return "the deferred spawn failed";
        case SpawnFail::FinishFailed:  return "finishing the spawn failed (the orphan was destroyed)";
        case SpawnFail::KeyMismatch:   return "the mirror was born under another key (destroyed)";
    }
    return "unknown";
}

// The key is written as a raw FName into the deferred actor, before its finish: the construction script
// runs in the finish and, finding a key, keeps it and registers it; finding none, it mints a random one.
// A raw write because ATV_C's own setKey is an empty stub in the Blueprint (its entry is a bare return),
// so the props lane's setKey call (coop/props/prop_fresh_spawn.cpp, the deferred key write) writes nothing on
// an ATV. The offset and the FName are resolved before anything is spawned; a miss spawns nothing,
// never a mirror with a random key. A failure sets `why` and logs nothing: the caller says it once per key.
void* SpawnMirror(const std::wstring& className, const FVector& loc, const FRotator& rot,
                  const std::wstring& key, SpawnFail* why, std::wstring* bornKey) {
    SpawnFail local = SpawnFail::None;
    SpawnFail& fail = why ? *why : local;
    fail = SpawnFail::None;
    if (!EnsureResolved()) { fail = SpawnFail::Unresolved; return nullptr; }
    if (className.empty() || key.empty()) { fail = SpawnFail::NoKey; return nullptr; }
    void* actorClass = R::FindClass(className.c_str());
    if (!actorClass) { fail = SpawnFail::ClassNotFound; return nullptr; }
    // Trust boundary: the className arrives over the wire -- only spawn ATV-lineage classes.
    void* atvCls = AtvClass();
    if (!atvCls || !R::IsDescendantOfAny(actorClass, &atvCls, 1)) { fail = SpawnFail::NotAnAtv; return nullptr; }
    const int32_t keyOff = R::FindPropertyOffset(actorClass, L"key");
    if (keyOff < 0) { fail = SpawnFail::NoOffset; return nullptr; }
    const R::FName keyName = fname_utils::StringToFName(key);
    if (keyName.ComparisonIndex == 0) { fail = SpawnFail::NameNone; return nullptr; }
    // GameplayStatics spawn refs (resolved once; same pair npc_mirror drives).
    static void* sGsCdo   = nullptr;
    static void* sBeginFn = nullptr;
    static void* sFinishFn = nullptr;
    if (!sGsCdo) sGsCdo = R::FindClassDefaultObject(L"GameplayStatics");
    if (sGsCdo && (!sBeginFn || !sFinishFn)) {
        if (void* gc = R::FindClass(L"GameplayStatics")) {
            sBeginFn  = R::FindFunction(gc, L"BeginDeferredActorSpawnFromClass");
            sFinishFn = R::FindFunction(gc, L"FinishSpawningActor");
        }
    }
    if (!sGsCdo || !sBeginFn || !sFinishFn) {
        static bool sWarned = false;
        if (!sWarned) { sWarned = true;
            UE_LOGW("atv: SpawnMirror -- GameplayStatics BeginDeferred/Finish unresolved; cannot spawn an ATV mirror"); }
        fail = SpawnFail::Unresolved;
        return nullptr;
    }
    void* worldCtx = engine::GetWorldContext();
    if (!worldCtx) { fail = SpawnFail::Unresolved; return nullptr; }

    FTransform xform{};
    engine::RotatorToQuat(rot.Pitch, rot.Yaw, rot.Roll, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc.X; xform.TY = loc.Y; xform.TZ = loc.Z;  // scale stays unit (ctor default)

    void* spawned = nullptr;
    {
        ParamFrame begin(sBeginFn);
        if (!begin.valid()) { fail = SpawnFail::BeginFailed; return nullptr; }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", uint8_t{1});  // AlwaysSpawn
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(sGsCdo, begin)) { fail = SpawnFail::BeginFailed; return nullptr; }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) { fail = SpawnFail::BeginFailed; return nullptr; }
    // While the actor is still deferred: its construction script runs in the finish.
    *reinterpret_cast<R::FName*>(reinterpret_cast<char*>(spawned) + keyOff) = keyName;
    {
        ParamFrame finish(sFinishFn);
        if (!finish.valid()) {
            // The BeginDeferred actor was created but never finished -- UE does NOT GC it (it lingers
            // in the world). Destroy the orphan, exactly as npc_mirror does on the same failure.
            DestroyMirror(spawned);
            fail = SpawnFail::FinishFailed;
            return nullptr;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(sGsCdo, finish)) {
            DestroyMirror(spawned);  // don't leak the half-spawned actor
            fail = SpawnFail::FinishFailed;
            return nullptr;
        }
    }
    // The construction script's keeping of a preset key is read back, not assumed: a mirror that stands
    // under another key, a case-different one included (the index and the prune file key exactly), would
    // be filed by the caller under the host's and be pruned as a stranger by the next pass. The same rig
    // destroy as the failures above.
    const std::wstring born = GetKeyString(spawned);
    if (born != key) {
        if (bornKey) *bornKey = born;
        DestroyMirror(spawned);
        fail = SpawnFail::KeyMismatch;
        return nullptr;
    }
    return spawned;  // physics LEFT ON -- a native idle ATV (grabbable). atv_sync owns lifetime/sync.
}

bool Dismount(void* atv) {
    if (!atv || !R::IsLive(atv)) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(atv), L"dismount");
    ParamFrame f(fn);
    return fn && f.valid() && Call(atv, f);
}

bool DestroyMirror(void* atv) {
    if (!atv || !R::IsLive(atv)) return false;
    // A driver this peer's controller holds in it is handed back first, through the ATV's own dismount,
    // which re-possesses and shows the player as a driver's own leaving does: the rig possesses the ATV
    // while driven, so destroying it seated would leave the player hidden and unpossessed. MTA's vehicle
    // removes its driver and passengers before its game vehicle goes
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientVehicle.cpp:3075-3094). A dismount that
    // cannot run is said: the destroy still goes ahead, and the player is left as the seat held them.
    if (engine::GetController(atv) && !Dismount(atv))
        UE_LOGW("atv: DestroyMirror could not run the ATV's own dismount -- the local driver stays "
                "unpossessed and hidden after the ATV goes");
    // K2_DestroyActor is declared on Actor, not on ATV_C, so it is resolved there.
    return engine::DestroyActor(atv);
}

namespace {

// A field of ATV_C by name, resolved once per field: the layout is the same in every world. -1 while ATV_C is not loaded
// (asked again) or when it has no such field.
struct Field {
    const wchar_t* name;
    int32_t off = -1;
    bool tried = false;
    int32_t Get() {
        if (!tried) {
            void* cls = AtvClass();
            if (!cls) return -1;
            off = R::FindPropertyOffset(cls, name);
            tried = true;
            if (off < 0) UE_LOGW("atv: ATV_C has no `%ls`", name);
        }
        return off;
    }
};
Field g_camera{L"Camera"}, g_sitarrow{L"sitarrow"}, g_sittingKerfuro{L"sittingKerfuro"}, g_allKerfuros{L"allKerfuros"};

void* ReadObj(void* atv, int32_t off) {
    if (!atv || off < 0) return nullptr;
    void* o = *reinterpret_cast<void* const*>(static_cast<const char*>(atv) + off);
    return o && R::IsLive(o) ? o : nullptr;
}

bool WriteKerfurList(void* atv, const std::vector<void*>& list) {
    const int32_t off = g_allKerfuros.Get();
    if (off < 0) return false;
    void* buf = list.empty() ? nullptr : save_record::AllocZeroed(list.size(), sizeof(void*));
    if (!list.empty() && !buf) return false;
    if (buf) std::memcpy(buf, list.data(), list.size() * sizeof(void*));
    save_record::WriteArrHeader(atv, off, buf, static_cast<int32_t>(list.size()));
    return true;
}

}  // namespace

bool GetSeatTransform(void* atv, FVector& loc, FRotator& rot) {
    if (!atv || !R::IsLive(atv)) return false;
    void* camera = ReadObj(atv, g_camera.Get());
    void* sit = ReadObj(atv, g_sitarrow.Get());
    FVector eye{}, seat{};
    if (!camera || !sit || !engine::TryGetComponentLocation(camera, eye) || !engine::TryGetComponentLocation(sit, seat) ||
        !engine::TryGetComponentWorldRotation(sit, rot))
        return false;
    // The camera stands at the back of the seat; the body sits forward of it, at the handlebars.
    constexpr float kForwardCm = 100.f;
    constexpr float kUpCm = 25.f;   // on the seat, not in it
    constexpr float kDeg2Rad = 0.01745329252f;
    const float yaw = rot.Yaw * kDeg2Rad, pitch = rot.Pitch * kDeg2Rad;
    loc = FVector{eye.X + std::cos(pitch) * std::cos(yaw) * kForwardCm,
                  eye.Y + std::cos(pitch) * std::sin(yaw) * kForwardCm,
                  seat.Z + std::sin(pitch) * kForwardCm + kUpCm};
    return true;
}

void* NearestAtv(const FVector& at, float reachCm) {
    struct Best { FVector at; float d2; void* atv; } best{at, reachCm * reachCm, nullptr};
    ForEachAtv([](void* ctx, void* actor, int32_t) {
        auto* b = static_cast<Best*>(ctx);
        FVector loc{};
        if (!engine::TryGetActorLocation(actor, loc)) return;
        const float dx = loc.X - b->at.X, dy = loc.Y - b->at.Y, dz = loc.Z - b->at.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= b->d2) { b->d2 = d2; b->atv = actor; }
    }, &best);
    return best.atv;
}

void ForgetSittingKerfur(void* atv, void* kerfur) {
    const int32_t off = g_sittingKerfuro.Get();
    if (!atv || !kerfur || !R::IsLive(atv) || off < 0) return;
    void** slot = reinterpret_cast<void**>(static_cast<char*>(atv) + off);
    if (*slot == kerfur) *slot = nullptr;
}

bool InviteKerfurs(void* atv, const std::vector<void*>& kerfurs) {
    if (!atv || !R::IsLive(atv) || !WriteKerfurList(atv, kerfurs)) return false;
    for (void* k : kerfurs) kerfur::TryToOccupyCar(k, atv);
    return true;
}

bool ReleaseKerfurs(void* atv) {
    if (!atv || !R::IsLive(atv)) return false;
    const int32_t listOff = g_allKerfuros.Get(), riderOff = g_sittingKerfuro.Get();
    if (listOff < 0 || riderOff < 0) return false;
    if (void* rider = ReadObj(atv, riderOff)) kerfur::GetOffCar(rider);
    const save_record::Arr arr = save_record::ReadArr(atv, listOff);
    std::vector<void*> rest;
    for (int32_t i = 0; arr.data && i < arr.num && i < 256; ++i)
        rest.push_back(reinterpret_cast<void* const*>(arr.data)[i]);
    for (void* k : rest)
        if (k && R::IsLive(k)) kerfur::FailCar(k);
    *reinterpret_cast<void**>(static_cast<char*>(atv) + riderOff) = nullptr;
    return WriteKerfurList(atv, {});
}

}  // namespace ue_wrap::atv
