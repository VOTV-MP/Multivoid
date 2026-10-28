// ue_wrap/actors/geom_octa.cpp -- see ue_wrap/actors/geom_octa.h.

#include "ue_wrap/actors/geom_octa.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/reflection.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ue_wrap::geom_octa {
namespace {

namespace R = reflection;

// The class's export names: minSphere's begin-overlap handler, and the "fire" section's finish notify.
constexpr const wchar_t* kMinSphereOverlap =
    L"BndEvt__geomOcta_minSphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";
constexpr const wchar_t* kFireFinish = L"OnNotifyBegin_1DFC28904024A8FB71C3ACA9DBD440F9";

R::InstanceOffset g_target{L"targetActor"};
R::InstanceOffset g_owner{L"Owner"};
R::InstanceOffset g_minSphere{L"minSphere"};

// The live object in the pointer slot `off` bytes into `base`, or null.
void* LivePointerAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + static_cast<size_t>(off));
    return (p && R::IsLive(p)) ? p : nullptr;
}

}  // namespace

void* TargetActor(void* octa) { return octa ? LivePointerAt(octa, g_target.Of(octa)) : nullptr; }

bool IsAnim(void* octa) {
    if (!octa) return false;
    // The member's place is kept per class object and found again when another class object comes (a reloaded world).
    static void*   cls = nullptr;
    static int32_t off = -1;
    static uint8_t mask = 0;
    void* const c = R::ClassOf(octa);
    if (c != cls) {
        cls = c;
        off = -1;
        mask = 0;
        if (!R::FindBoolProperty(c, L"isAnim", off, mask)) off = -1;
    }
    return off >= 0 && (*(static_cast<const uint8_t*>(octa) + off) & mask) != 0;
}

void* MinSphere(void* octa) { return octa ? LivePointerAt(octa, g_minSphere.Of(octa)) : nullptr; }

bool IsFinishNotify(const script_gate::Call& call) {
    static R::ParamOffset notifyName{L"NotifyName"};
    const int32_t off = notifyName.Of(call.function);
    if (off < 0 || !call.locals) return false;
    R::FName name{};
    std::memcpy(&name, call.locals + off, sizeof(name));
    return R::NameEquals(name, L"finish");
}

void* GraphFunction(void* octa) {
    return octa ? R::FindFunction(R::ClassOf(octa), L"ExecuteUbergraph_geomOcta") : nullptr;
}

bool CallMinSphereBeginOverlap(void* octa, void* otherActor) {
    void* fn = octa ? R::FindFunction(R::ClassOf(octa), kMinSphereOverlap) : nullptr;
    ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<void*>(L"OverlappedComponent", MinSphere(octa));
    f.Set<void*>(L"OtherActor", otherActor);
    return Call(octa, f);
}

bool CallFireFinishNotify(void* octa) {
    void* fn = octa ? R::FindFunction(R::ClassOf(octa), kFireFinish) : nullptr;
    ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    // The name `finish`, resolved once.
    static R::FName name{};
    if (name.ComparisonIndex == 0 && name.Number == 0) name = fname_utils::StringToFName(L"finish");
    return f.SetRaw(L"NotifyName", &name, sizeof(name)) && Call(octa, f);
}

void* LaserOwner(void* laser) { return laser ? LivePointerAt(laser, g_owner.Of(laser)) : nullptr; }

}  // namespace ue_wrap::geom_octa
