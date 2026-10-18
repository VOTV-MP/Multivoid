// ue_wrap/actors/fossilhound.cpp -- see ue_wrap/actors/fossilhound.h.

#include "ue_wrap/actors/fossilhound.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ue_wrap::fossilhound {
namespace {

namespace P = profile;
namespace R = reflection;

constexpr const wchar_t* kAnimClass = L"fossilhound_Skeleton_AnimBlueprint_C";

R::InstanceOffset g_springArm{L"SpringArm"};
R::InstanceOffset g_anim{L"anim"};
R::InstanceOffset g_owner{L"owner"};  // on the AnimBP: the creature whose velocity it reads

// EAttachmentRule::KeepRelative, which BeginPlay passes for all three rules.
constexpr uint8_t kKeepRelative = 0;

// A live object in the pointer slot `off` bytes into `base`, or null.
void* LiveAt(void* base, size_t off) {
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Says a step failed, once per step for this hound's class (keyed by the class's name, which a world change does
// not move). False, so a step's failure reads `ok = Failed(...)`.
bool Failed(void* hound, const char* step) {
    struct Seen {
        R::FName    cls;
        const char* step;
    };
    static Seen seen[8];
    static int count = 0;
    const R::FName& name = R::NameOf(R::ClassOf(hound));
    for (int i = 0; i < count; ++i)
        if (seen[i].cls.ComparisonIndex == name.ComparisonIndex && seen[i].cls.Number == name.Number &&
            std::strcmp(seen[i].step, step) == 0)
            return false;
    if (count < 8) seen[count++] = Seen{name, step};
    UE_LOGW("fossilhound: %ls mirror presentation step '%s' did not run", R::ClassNameOf(hound).c_str(), step);
    return false;
}

// Mesh->K2_AttachToComponent(SpringArm, None, KeepRelative x3, true).
bool AttachMesh(void* hound, void* mesh) {
    const int32_t off = g_springArm.Of(hound);
    void* arm = off >= 0 ? LiveAt(hound, static_cast<size_t>(off)) : nullptr;
    void* fn = arm ? R::FindDispatchFunctionCached(R::ClassOf(mesh), L"K2_AttachToComponent") : nullptr;
    ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName none{};
    f.Set<void*>(L"Parent", arm);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", kKeepRelative);
    f.Set<uint8_t>(L"RotationRule", kKeepRelative);
    f.Set<uint8_t>(L"ScaleRule", kKeepRelative);
    f.Set<bool>(L"bWeldSimulatedBodies", true);
    return Call(mesh, f);
}

// this.anim = the mesh's animation instance, this.anim.owner = this, when that instance is the class's AnimBP.
// A mesh with another animation blueprint is the game's own failed cast: nothing to write.
bool WriteAnim(void* hound, void* mesh) {
    void* anim = LiveAt(mesh, P::off::USkeletalMesh_AnimScriptInstance);
    void* base = object_index::ClassByName(kAnimClass);
    void* animCls = anim ? R::ClassOf(anim) : nullptr;
    if (!animCls || !base || !R::IsDescendantOfAny(animCls, &base, 1)) return true;
    const int32_t animOff = g_anim.Of(hound);
    const int32_t ownerOff = g_owner.Of(anim);
    if (animOff < 0 || ownerOff < 0) return false;
    *reinterpret_cast<void**>(static_cast<uint8_t*>(hound) + animOff) = anim;
    *reinterpret_cast<void**>(static_cast<uint8_t*>(anim) + ownerOff) = hound;
    return true;
}

// ExecuteUbergraph_fossilhound(EntryPoint = kPerViewerEntry). The function and its EntryPoint parameter resolve,
// or the call is not made.
bool EnterPerViewerLoop(void* hound) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(hound), L"ExecuteUbergraph_fossilhound");
    if (!fn || R::FindParamOffset(fn, L"EntryPoint") < 0) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<int32_t>(L"EntryPoint", kPerViewerEntry) && Call(hound, f);
}

}  // namespace

bool ApplyBeginPlayPresentation(void* hound) {
    if (!hound || !R::IsLive(hound)) return false;
    bool ok = true;
    void* mesh = LiveAt(hound, P::off::ACharacter_Mesh);
    if (!mesh) {
        ok = Failed(hound, "mesh");
    } else {
        if (!AttachMesh(hound, mesh)) ok = Failed(hound, "attach");
        if (!WriteAnim(hound, mesh)) ok = Failed(hound, "anim");
    }
    if (!EnterPerViewerLoop(hound)) ok = Failed(hound, "per-viewer loop");
    return ok;
}

}  // namespace ue_wrap::fossilhound
