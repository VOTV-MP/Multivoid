// ue_wrap/devices/cremator.cpp -- see ue_wrap/devices/cremator.h.

#include "ue_wrap/devices/cremator.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::cremator {

// The function whose body holds the loop that burns the actors the machine collected: LoopActor reads its ubergraph.
UE_FACT_FN(kRowIgniteInside, "ue_wrap.cremator", L"cremator_C", L"igniteInside", Declared);

namespace {

namespace R  = reflection;
namespace OI = object_index;

// cremator_C as this world holds it, looked up by name again once its slot or serial no longer holds the kept
// one (a Blueprint class can be a new object in a new world).
CachedObjRef g_cls;

void* ClassFor() {
    if (void* k = g_cls.Get()) return k;
    void* k = OI::ClassByName(L"cremator_C");
    if (k) g_cls.Set(k);
    return k;
}

// A Blueprint bool is an FBoolProperty: its byte offset and the bit inside that byte, since several pack into one
// byte and a raw byte read would read a neighbour's bit. Found once per class object, again for a new one.
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
};
BoolField g_active{L"isActive"};
BoolField g_anim{L"anim"};
BoolField g_closed{L"isClosed"};
BoolField g_lookingHandle{L"lookingAt_handle"};
BoolField g_lookingValve{L"lookingAt_valve"};
BoolField g_doorOpened{L"opened"};

R::InstanceOffset g_windup{L"windup"};
R::InstanceOffset g_gas{L"gasAmount"};
R::InstanceOffset g_actors{L"actors"};
R::InstanceOffset g_health{L"health"};
R::InstanceOffset g_door{L"swingerDoor"};
R::InstanceOffset g_firebox{L"firebox"};
R::InstanceOffset g_burning{L"burning"};
R::InstanceOffset g_handle{L"handle"};

bool ReadBoolOn(void* obj, void* cls, BoolField& f, bool& out) {
    if (!obj || !cls) return false;
    if (f.cls != cls) {
        f.cls = cls;
        f.off = -1;
        f.mask = 0;
        if (!R::FindBoolProperty(cls, f.name, f.off, f.mask) || f.off < 0 || !f.mask) f.off = -1;
    }
    if (f.off < 0) return false;
    out = (*(reinterpret_cast<const uint8_t*>(obj) + f.off) & f.mask) != 0;
    return true;
}

bool ReadFlag(void* c, BoolField& f, bool& out) {
    return IsCremator(c) && ReadBoolOn(c, R::ClassOf(c), f, out);
}

// Sets or clears the field's bit; the read resolves the field first. False when the field did not resolve.
bool WriteFlag(void* c, BoolField& f, bool value) {
    bool unused = false;
    if (!ReadFlag(c, f, unused)) return false;
    uint8_t& byte = *(reinterpret_cast<uint8_t*>(c) + f.off);
    byte = value ? static_cast<uint8_t>(byte | f.mask) : static_cast<uint8_t>(byte & ~f.mask);
    return true;
}

bool ReadFloat(void* c, R::InstanceOffset& off, float& out) {
    if (!IsCremator(c)) return false;
    const int32_t o = off.Of(c);
    if (o < 0) return false;
    std::memcpy(&out, static_cast<const char*>(c) + o, sizeof(out));
    return true;
}

void* ReadObject(void* c, R::InstanceOffset& off) {
    if (!IsCremator(c)) return nullptr;
    const int32_t o = off.Of(c);
    if (o < 0) return nullptr;
    void* value = nullptr;
    std::memcpy(&value, static_cast<const char*>(c) + o, sizeof(value));
    return value && R::IsLive(value) ? value : nullptr;
}

}  // namespace

bool EnsureResolved() { return ClassFor() != nullptr; }

bool IsCremator(void* obj) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    return cls && cls == ClassFor();
}


std::wstring GetKeyString(void* c) { return IsCremator(c) ? prop::GetActorSaveKeyString(c) : std::wstring(); }

bool ReadActive(void* c, bool& v) { return ReadFlag(c, g_active, v); }
bool ReadAnim(void* c, bool& v) { return ReadFlag(c, g_anim, v); }
bool ReadClosed(void* c, bool& v) { return ReadFlag(c, g_closed, v); }
bool ReadLookingAtHandle(void* c, bool& v) { return ReadFlag(c, g_lookingHandle, v); }
bool ReadLookingAtValve(void* c, bool& v) { return ReadFlag(c, g_lookingValve, v); }
bool WriteLookingAtHandle(void* c, bool v) { return WriteFlag(c, g_lookingHandle, v); }
bool ReadWindup(void* c, float& v) { return ReadFloat(c, g_windup, v); }
bool ReadGasAmount(void* c, float& v) { return ReadFloat(c, g_gas, v); }

Machine ReadMachine(void* c) {
    Machine m;
    if (!IsCremator(c)) return m;
    // A TArray is { void* Data; int32 Num; int32 Max }.
    const int32_t actorsOff = g_actors.Of(c);
    if (actorsOff >= 0)
        std::memcpy(&m.actors, static_cast<const char*>(c) + actorsOff + sizeof(void*), sizeof(int32_t));
    const int32_t healthOff = g_health.Of(c);
    if (healthOff < 0) return m;
    void* data = nullptr;
    int32_t num = 0;
    std::memcpy(&data, static_cast<const char*>(c) + healthOff, sizeof(data));
    std::memcpy(&num, static_cast<const char*>(c) + healthOff + sizeof(void*), sizeof(num));
    if (num > 0 && data) std::memcpy(&m.health0, data, sizeof(m.health0));
    return m;
}

void* Door(void* c) { return ReadObject(c, g_door); }
void* Firebox(void* c) { return ReadObject(c, g_firebox); }
void* BurnVolume(void* c) { return ReadObject(c, g_burning); }
void* Handle(void* c) { return ReadObject(c, g_handle); }

bool HandleLocation(void* c, FVector& at) {
    void* handle = Handle(c);
    return handle && engine::TryGetComponentLocation(handle, at);
}

bool OverlapsBurnVolume(void* c, void* actor) {
    void* const volume = BurnVolume(c);
    if (!volume || !actor || !R::IsLive(actor)) return false;
    // IsOverlappingActor is UPrimitiveComponent's; the dispatch cache climbs to it.
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(volume), L"IsOverlappingActor");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Other", actor);
    return Call(volume, f) && f.Get<bool>(L"ReturnValue");
}

bool ReadDoorOpened(void* c, bool& opened) {
    void* door = Door(c);
    return door && ReadBoolOn(door, R::ClassOf(door), g_doorOpened, opened);
}

bool FireboxLocation(void* c, FVector& at) {
    void* firebox = Firebox(c);
    return firebox && engine::TryGetComponentLocation(firebox, at);
}

bool LoopActor(void* callerFunction, const uint8_t* callerLocals, void*& actor) {
    // Resolved per class object, and again when the ubergraph held is no longer that class's live function: a world
    // that reloads the class brings a new function, possibly at the old class's address.
    static void*   sCls = nullptr;
    static void*   sUber = nullptr;
    static int32_t sUberIdx = -1;
    static int32_t sOff = -1;
    actor = nullptr;
    void* cls = ClassFor();
    if (!cls || !callerFunction || !callerLocals) return false;
    if (cls != sCls || (sUber && (!R::IsLiveByIndex(sUber, sUberIdx) || R::OuterOf(sUber) != cls))) {
        sCls = cls;
        sUber = R::FindFunction(cls, L"ExecuteUbergraph_cremator");
        sUberIdx = sUber ? R::InternalIndexOf(sUber) : -1;
        sOff = sUber && facts::ResolveOn(kRowIgniteInside, cls) ? R::FindPropertyOffset(sUber, L"CallFunc_Array_Get_Item") : -1;
        if (sOff < 0)
            UE_LOGW("cremator: the burn loop's actor variable did not resolve (ubergraph %p) -- the loop's actor cannot be "
                    "read while this class lives", sUber);
    }
    if (sOff < 0 || callerFunction != sUber) return false;
    std::memcpy(&actor, callerLocals + sOff, sizeof(actor));
    return actor != nullptr;
}

bool CallUseLever(void* c) {
    if (!IsCremator(c)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(c), L"useLever");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(c, f);
}

bool ShutDoor(void* c, bool& wasOpened) {
    wasOpened = false;
    void* door = Door(c);
    return !(door && ReadDoorOpened(c, wasOpened) && wasOpened) || swinger::CallClose(door);
}

bool OpenDoor(void* c, bool& opened) {
    opened = false;
    void* door = Door(c);
    if (!door) return false;
    // Open(damage): damage false is the door's own player path.
    const bool ran = (ReadDoorOpened(c, opened) && opened) || swinger::CallOpen(door, false);
    ReadDoorOpened(c, opened);
    return ran;
}

Nearest FindNearestFirebox(const FVector& from) {
    Nearest n{from, INFINITY, nullptr, {}};
    void* cls = ClassFor();
    if (!cls) return n;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Nearest*>(ctx);
        FVector at{};
        if (!FireboxLocation(obj, at)) return;
        const float dx = at.X - s.from.X, dy = at.Y - s.from.Y, dz = at.Z - s.from.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < s.best) { s.best = d2; s.actor = obj; s.at = at; }
    }, &n);
    return n;
}

MoveResult FindFireboxMove(const FVector& from, FireboxMove& out) {
    out = FireboxMove{};
    const Nearest n = FindNearestFirebox(from);
    if (!n.actor) return MoveResult::NoCremator;
    out.actor = n.actor;
    out.at = n.at;
    return engine::TryGetActorRotation(n.actor, out.rot) ? MoveResult::Found : MoveResult::RotationUnread;
}

}  // namespace ue_wrap::cremator
