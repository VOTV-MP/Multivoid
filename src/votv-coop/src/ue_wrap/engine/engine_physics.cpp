// ue_wrap/engine/engine_physics.cpp -- see ue_wrap/engine/engine_physics.h.

#include "ue_wrap/engine/engine_physics.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// Every frame here fits 64 bytes: SetSimulatePhysics takes 1, the two velocity setters 24,
// IsSimulatingPhysics 12. A UFunction whose frame outgrows it is refused at the resolve, so a
// recooked signature can never make ProcessEvent write past the buffer.
constexpr int32_t kFrameBytes = 64;

// One UFunction and the offsets of the two parameters its call writes or reads.
struct Thunk {
    const wchar_t* name;
    const wchar_t* param0;
    const wchar_t* param1;  // nullptr for a one-parameter call
    void*   fn = nullptr;
    ue_wrap::CachedObjRef cls;   // the class `fn` was found on, by slot and serial
    int32_t off0 = -1;
    int32_t off1 = -1;
    bool    missed = false;  // said once; the call is a no-op from then on
};

Thunk g_setSimulate{P::name::SetSimulatePhysicsFn, L"bSimulate", nullptr};
Thunk g_setLinVel{P::name::SetPhysicsLinearVelocityFn, L"NewVel", L"BoneName"};
Thunk g_setAngVel{P::name::SetPhysicsAngularVelocityInDegreesFn, L"NewAngVel", L"BoneName"};
Thunk g_isSimulating{P::name::IsSimulatingPhysicsFn, L"BoneName", L"ReturnValue"};

// The class they are found on. A native class outlives every world, but the references are checked:
// a freed one is found again, and each thunk, holding the class it resolved on by slot and serial,
// resolves again, even when the new class sits at the old address. A native class is found at boot or
// never, so a miss is said once like a function's.
ue_wrap::CachedObjRef g_primitiveClass;

// Found up the class chain: IsSimulatingPhysics is declared on USceneComponent.
bool Resolve(Thunk& t) {
    if (t.missed) return false;
    if (!g_primitiveClass.Alive()) g_primitiveClass.Set(R::FindClass(P::name::PrimitiveComponentClass));
    void* cls = g_primitiveClass.Raw();
    if (t.fn && cls && t.cls.Alive() && t.cls.Raw() == cls) return true;
    t.cls.Set(cls);
    t.fn = cls ? R::FindDispatchFunction(cls, t.name, nullptr) : nullptr;
    const int32_t frame = t.fn ? R::FunctionFrameSize(t.fn) : 0;
    t.off0 = t.fn ? R::FindParamOffset(t.fn, t.param0) : -1;
    t.off1 = (t.fn && t.param1) ? R::FindParamOffset(t.fn, t.param1) : -1;
    if (!t.fn || frame > kFrameBytes || t.off0 < 0 || (t.param1 && t.off1 < 0)) {
        UE_LOGW("engine_physics: %ls did not resolve (class=%p fn=%p frame=%d %ls@%d) -- its call is a "
                "no-op", t.name, cls, t.fn, frame, t.param0, t.off0);
        t.fn = nullptr;
        t.missed = true;
        return false;
    }
    UE_LOGI("engine_physics: resolved %ls (frame=%d %ls@%d)", t.name, frame, t.param0, t.off0);
    return true;
}

// The component members of a class, found once per class name: its own and its generated parents', up to the
// first native class, whose members are not variables. A Blueprint's component variable is an instanced object
// member of pointer size. A deque, so an entry handed out stays where it is when another is added.
struct ClassMembers {
    R::FName name;
    std::vector<int32_t> offs;
};
std::deque<ClassMembers> g_componentMembers;  // a handful of classes

const std::vector<int32_t>& ComponentMembers(void* cls) {
    const R::FName& nm = R::NameOf(cls);
    for (const ClassMembers& m : g_componentMembers)
        if (m.name.ComparisonIndex == nm.ComparisonIndex && m.name.Number == nm.Number) return m.offs;
    ClassMembers& m = g_componentMembers.emplace_back(ClassMembers{nm, {}});
    for (void* c = cls; c; c = R::SuperStructOf(c)) {
        const std::wstring n = R::ToString(R::NameOf(c));
        if (n.size() < 2 || n.compare(n.size() - 2, 2, L"_C") != 0) break;
        for (const R::StructFieldInfo& f : R::EnumerateStructFields(c))
            if ((f.flags & P::cpf::InstancedReference) && f.size == static_cast<int32_t>(sizeof(void*)))
                m.offs.push_back(f.offset);
    }
    return m.offs;
}

void SetVelocity(Thunk& t, void* component, float x, float y, float z) {
    if (!component || !Resolve(t)) return;
    unsigned char frame[kFrameBytes] = {};
    *reinterpret_cast<FVector*>(frame + t.off0) = FVector{x, y, z};
    *reinterpret_cast<R::FName*>(frame + t.off1) = R::FName{0, 0};  // None: the whole body
    R::CallFunction(component, t.fn, frame);
}

}  // namespace

void SetComponentSimulatePhysics(void* component, bool simulate) {
    if (!component || !Resolve(g_setSimulate)) return;
    unsigned char frame[kFrameBytes] = {};
    *reinterpret_cast<bool*>(frame + g_setSimulate.off0) = simulate;
    R::CallFunction(component, g_setSimulate.fn, frame);
}

void SetComponentLinearVelocity(void* component, float vx, float vy, float vz) {
    SetVelocity(g_setLinVel, component, vx, vy, vz);
}

void SetComponentAngularVelocity(void* component, float wx, float wy, float wz) {
    SetVelocity(g_setAngVel, component, wx, wy, wz);
}

bool IsComponentSimulatingPhysics(void* component) {
    if (!component || !Resolve(g_isSimulating)) return false;
    unsigned char frame[kFrameBytes] = {};
    *reinterpret_cast<R::FName*>(frame + g_isSimulating.off0) = R::FName{0, 0};  // None: the whole body
    R::CallFunction(component, g_isSimulating.fn, frame);
    return *reinterpret_cast<bool*>(frame + g_isSimulating.off1);
}

int StopActorSimulating(void* actor) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return 0;
    if (!g_primitiveClass.Alive()) g_primitiveClass.Set(R::FindClass(P::name::PrimitiveComponentClass));
    void* prim = g_primitiveClass.Raw();
    if (!prim) return 0;
    int stopped = 0;
    for (int32_t off : ComponentMembers(cls)) {
        void* comp = *reinterpret_cast<void* const*>(static_cast<const char*>(actor) + off);
        // Only the actor's own: a variable can hold another actor's component.
        if (!comp || !R::IsLive(comp) || R::OuterOf(comp) != actor) continue;
        void* compCls = R::ClassOf(comp);
        if (!compCls || !R::IsDescendantOfAny(compCls, &prim, 1)) continue;
        if (!IsComponentSimulatingPhysics(comp)) continue;
        SetComponentSimulatePhysics(comp, false);
        ++stopped;
    }
    return stopped;
}

}  // namespace ue_wrap::engine
