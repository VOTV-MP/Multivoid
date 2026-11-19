// ue_wrap/actors/fire_extinguisher.cpp -- see ue_wrap/actors/fire_extinguisher.h.

#include "ue_wrap/actors/fire_extinguisher.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cwchar>

namespace ue_wrap::fire_extinguisher {
namespace {

namespace R = reflection;
namespace P = profile;

R::FName g_extName{0, 0};
R::FName g_mountName{0, 0};
bool     g_namesResolved = false;

// A member's place in the instance, read off the first live object of its class.
struct Field {
    int32_t offset = -1;
    uint8_t mask = 0;   // a bool's mask; 0 for a non-bool
    bool    tried = false;
};
Field g_mountHeld, g_charge, g_spraying, g_thrusting;
Field g_compActive;  // UActorComponent::bIsActive, the bit a component's IsActive returns; one place for every component class

// The member components by property name: each one's offset is kept per class, for the next instance of that class.
struct MemberSlot {
    const wchar_t*        name;
    R::InstanceOffset     offset;
};
MemberSlot g_members[] = {
    {P::name::FireExtSprayEffectProp, R::InstanceOffset{P::name::FireExtSprayEffectProp}},
    {P::name::FireExtThrustEffectProp, R::InstanceOffset{P::name::FireExtThrustEffectProp}},
    {P::name::FireExtThrustSoundProp, R::InstanceOffset{P::name::FireExtThrustSoundProp}},
    {P::name::FireExtThrusterProp, R::InstanceOffset{P::name::FireExtThrusterProp}},
};

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

bool IsA(void* obj, const R::FName& name) {
    if (!obj || !g_namesResolved) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (SameName(R::NameOf(cls), name)) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

bool Resolve(Field& m, void* obj, const wchar_t* name, bool isBool) {
    if (m.offset >= 0) return true;
    if (m.tried) return false;
    m.tried = true;
    void* cls = R::ClassOf(obj);
    if (isBool) {
        int32_t off = -1;
        uint8_t mask = 0;
        if (!R::FindBoolProperty(cls, name, off, mask) || off < 0 || mask == 0) return false;
        m.offset = off;
        m.mask = mask;
        return true;
    }
    const int32_t off = R::FindPropertyOffset(cls, name);
    if (off < 0) return false;
    m.offset = off;
    return true;
}

bool ReadBool(void* ext, Field& m, const wchar_t* name, bool& out) {
    if (!IsExtinguisher(ext) || !Resolve(m, ext, name, /*isBool=*/true)) return false;
    out = (reinterpret_cast<const uint8_t*>(ext)[m.offset] & m.mask) != 0;
    return true;
}

bool WriteBool(void* ext, Field& m, const wchar_t* name, bool value) {
    if (!IsExtinguisher(ext) || !Resolve(m, ext, name, /*isBool=*/true)) return false;
    uint8_t& b = reinterpret_cast<uint8_t*>(ext)[m.offset];
    b = value ? static_cast<uint8_t>(b | m.mask) : static_cast<uint8_t>(b & ~m.mask);
    return true;
}

}  // namespace

bool ResolveNames() {
    if (g_namesResolved) return true;
    const R::FName ext = fname_utils::StringToFName(P::name::FireExtinguisherClass);
    const R::FName mount = fname_utils::StringToFName(P::name::FireExtinguisherMountClass);
    if (ext.ComparisonIndex == 0 || mount.ComparisonIndex == 0) return false;
    g_extName = ext;
    g_mountName = mount;
    g_namesResolved = true;
    return true;
}

bool NamesResolved() { return g_namesResolved; }

bool IsExtinguisher(void* obj) { return IsA(obj, g_extName); }
bool IsMount(void* obj) { return IsA(obj, g_mountName); }

void* MountedExtinguisher(void* mount) {
    if (!IsMount(mount) || !Resolve(g_mountHeld, mount, P::name::FireExtMountHeldProp, false))
        return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(mount) + g_mountHeld.offset);
}

bool ReadCharge(void* ext, float& out) {
    if (!IsExtinguisher(ext) || !Resolve(g_charge, ext, P::name::FireExtChargeProp, false)) return false;
    out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(ext) + g_charge.offset);
    return true;
}

bool ReadSpraying(void* ext, bool& out) {
    return ReadBool(ext, g_spraying, P::name::FireExtSprayingProp, out);
}

bool ReadThrusting(void* ext, bool& out) {
    return ReadBool(ext, g_thrusting, P::name::FireExtThrustingProp, out);
}

bool ReadComponentRotation(void* ext, const wchar_t* member, FRotator& out) {
    if (!IsExtinguisher(ext)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(ext), member);
    if (off < 0) return false;
    void* comp = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(ext) + off);
    return comp && engine::TryGetComponentWorldRotation(comp, out);
}

bool WriteCharge(void* ext, float life) {
    if (!IsExtinguisher(ext) || !Resolve(g_charge, ext, P::name::FireExtChargeProp, false)) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ext) + g_charge.offset) = life;
    return true;
}

bool WriteSpraying(void* ext, bool on) { return WriteBool(ext, g_spraying, P::name::FireExtSprayingProp, on); }

bool WriteThrusting(void* ext, bool on) { return WriteBool(ext, g_thrusting, P::name::FireExtThrustingProp, on); }

void* Member(void* ext, const wchar_t* name) {
    if (!name || !IsExtinguisher(ext)) return nullptr;
    for (MemberSlot& m : g_members) {
        if (std::wcscmp(m.name, name) != 0) continue;
        const int32_t off = m.offset.Of(ext);
        if (off < 0) return nullptr;
        void* const comp = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(ext) + off);
        return comp && R::IsLive(comp) ? comp : nullptr;
    }
    return nullptr;
}

bool ReadThrusterOn(void* ext, bool& out) {
    void* const thruster = Member(ext, P::name::FireExtThrusterProp);
    if (!thruster || !Resolve(g_compActive, thruster, L"bIsActive", /*isBool=*/true)) return false;
    out = (reinterpret_cast<const uint8_t*>(thruster)[g_compActive.offset] & g_compActive.mask) != 0;
    return true;
}

bool CastExtinguishRay(void* ext, const FVector& start, const FVector& end, float radius) {
    if (!IsExtinguisher(ext)) return false;
    // A frame per tick would allocate once per remote spray per frame: the frame is kept for the function it was built for and
    // rebuilt when the class's function is another object (a Blueprint class dies with its world). The three inputs are all
    // written again each call and the function has no output.
    static ParamFrame frame{nullptr};
    static CachedObjRef frameFn;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(ext), L"extinguishRay");
    if (!fn) return false;
    if (!frameFn.Is(fn)) {
        frame = ParamFrame(fn);
        frameFn.Set(fn);
    }
    return frame.valid() && frame.Set<FVector>(L"inputPin", start) && frame.Set<FVector>(L"inputPin2", end) &&
           frame.Set<float>(L"radius", radius) && Call(ext, frame);
}

bool StartRunaway(void* ext) {
    if (!IsExtinguisher(ext)) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(ext), L"fireDamage"));
    return f.valid() && f.Set<float>(L"damage", 0.f) && Call(ext, f);
}

}  // namespace ue_wrap::fire_extinguisher
