// ue_wrap/actors/prop_pry.cpp -- see ue_wrap/actors/prop_pry.h.

#include "ue_wrap/actors/prop_pry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_physics.h"

namespace ue_wrap::prop_pry {
namespace {

namespace R = reflection;

constexpr const wchar_t* kPlankClass = L"prop_pryablePlank_C";

// prop_pryablePlank_C::crowbarOpen's kick: the crowbar's right vector times this, in deg/s.
constexpr float kPlankKickDegS = 150.f;

bool IsA(void* obj, const wchar_t* className) {
    void* base = R::FindClass(className);
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

}  // namespace

Kind KindOf(void* obj) {
    if (!obj || !R::IsLive(obj)) return Kind::None;
    return IsA(obj, kPlankClass) ? Kind::Plank : Kind::None;
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

}  // namespace ue_wrap::prop_pry
