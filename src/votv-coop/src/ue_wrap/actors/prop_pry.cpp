// ue_wrap/actors/prop_pry.cpp -- see ue_wrap/actors/prop_pry.h.

#include "ue_wrap/actors/prop_pry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_physics.h"

#include <chrono>
#include <cstdint>

namespace ue_wrap::prop_pry {
namespace {

namespace R = reflection;

constexpr const wchar_t* kPlankClass    = L"prop_pryablePlank_C";
constexpr const wchar_t* kRadiatorClass = L"radiator_C";
constexpr const wchar_t* kTvClass       = L"securityTv_C";

// prop_pryablePlank_C::crowbarOpen's kick: the crowbar's right vector times this, in deg/s.
constexpr float kPlankKickDegS = 150.f;

R::InstanceOffset g_joint[kJoints] = {R::InstanceOffset(L"PhysicsConstraint1"), R::InstanceOffset(L"PhysicsConstraint2"),
                                      R::InstanceOffset(L"PhysicsConstraint3"), R::InstanceOffset(L"PhysicsConstraint4")};
R::InstanceOffset g_audioObject(L"audio_object");

// The pryable base classes, each resolved once and held by slot and serial (a class that dies with its world is found
// again), a null read asked again at most once a second. Their children -- the plank's edges-only variant, the placed
// radiator_base1_C and radiator_base2_C -- are found by the descendant test, so no child is resolved of its own.
struct Pryable {
    const wchar_t* name;
    Kind kind;
    ue_wrap::CachedObjRef cls;
    int64_t nextTryMs = 0;
};
Pryable g_pryable[3] = {{kPlankClass, Kind::Plank}, {kRadiatorClass, Kind::Hanging}, {kTvClass, Kind::Hanging}};

void* ResolveClass(Pryable& p) {
    if (p.cls.Alive()) return p.cls.Raw();
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < p.nextTryMs) return nullptr;
    p.nextTryMs = now + 1000;
    p.cls.Set(R::FindClass(p.name));
    return p.cls.Alive() ? p.cls.Raw() : nullptr;
}


// The component the actor's variable at `off` holds, when it is a live one of the actor's own.
void* MemberAt(void* actor, int32_t off) {
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void* const*>(static_cast<const char*>(actor) + off);
    return (comp && R::IsLive(comp) && R::OuterOf(comp) == actor) ? comp : nullptr;
}

}  // namespace

Kind KindOf(void* obj) {
    if (!obj || !R::IsLive(obj)) return Kind::None;
    // BreakConstraint, which the lane's hook watches, is called by every ungrab and every ATV: pointer compares only.
    void* cls = R::ClassOf(obj);
    if (!cls) return Kind::None;
    for (Pryable& p : g_pryable) {
        void* base = ResolveClass(p);
        if (base && R::IsDescendantOfAny(cls, &base, 1)) return p.kind;
    }
    return Kind::None;
}

bool ReadRightVector(void* crowbar, float out[3]) {
    void* root = engine::RootComponentOf(crowbar);
    void* fn = root ? R::FindDispatchFunctionCached(R::ClassOf(root), L"GetRightVector") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(root, f)) return false;
    FVector v = f.Get<FVector>(L"ReturnValue");
    out[0] = v.X;
    out[1] = v.Y;
    out[2] = v.Z;
    return true;
}

bool FreePlank(void* prop, float x, float y, float z) {
    if (KindOf(prop) != Kind::Plank) return false;
    if (!prop::CallBaseSetPropProps(prop, false, false, false)) {
        UE_LOGW("prop_pry: setPropProps did not run on plank %p -- its pry is not applied", prop);
        return false;
    }
    void* mesh = prop::GetStaticMesh(prop);
    if (!engine::AddComponentAngularVelocity(mesh, x * kPlankKickDegS, y * kPlankKickDegS, z * kPlankKickDegS))
        UE_LOGW("prop_pry: the kick did not run on plank %p's body -- it falls without it", prop);
    return true;
}

bool CallBreakEffect(void* prop) {
    if (KindOf(prop) != Kind::Hanging) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"break");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(prop, f);
}

int JointOf(void* prop, void* constraint) {
    if (!prop || !constraint || KindOf(prop) != Kind::Hanging) return 0;
    for (int n = 1; n <= kJoints; ++n)
        if (MemberAt(prop, g_joint[n - 1].Of(prop)) == constraint) return n;
    return 0;
}

bool BreakJoint(void* prop, int joint, bool sound) {
    if (joint < 1 || joint > kJoints || KindOf(prop) != Kind::Hanging) return false;
    void* comp = MemberAt(prop, g_joint[joint - 1].Of(prop));
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"BreakConstraint") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(comp, f)) return false;
    if (!sound) return true;
    void* audio = MemberAt(prop, g_audioObject.Of(prop));
    void* play = audio ? R::FindDispatchFunctionCached(R::ClassOf(audio), L"Play") : nullptr;
    if (!play) return true;  // the joint is broken; the sound is a cue
    ParamFrame p(play);  // StartTime 0
    if (p.valid()) Call(audio, p);
    return true;
}

}  // namespace ue_wrap::prop_pry
