// ue_wrap/engine/hit_result.cpp -- see ue_wrap/engine/hit_result.h.

#include "ue_wrap/engine/hit_result.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ue_wrap::hit_result {
namespace {

namespace R = reflection;

// GameplayStatics is a native class: the same object for the process, as every native class was in
// the class-lifetime census (poll arc section 2.7), so its CDO, its function and the struct the function
// returns are kept.
void* g_cdo = nullptr;
void* g_makeFn = nullptr;
void* g_hitStruct = nullptr;  // FHitResult's UScriptStruct, the return value's type

// Does the built hit's weak pointer `field` resolve to `object`, through the object array, as the
// engine's own BreakHitResult reads it?
bool Names(const uint8_t* hit, const wchar_t* field, void* object) {
    const int32_t off = R::FindPropertyOffset(g_hitStruct, field);
    if (off < 0) return false;
    int32_t idx = 0, serial = 0;
    std::memcpy(&idx, hit + off, sizeof(idx));
    std::memcpy(&serial, hit + off + sizeof(idx), sizeof(serial));
    return R::ResolveWeakObject(idx, serial) == object;
}

}  // namespace

bool Write(ParamFrame& frame, const wchar_t* param, void* actor, void* component, const FVector& location) {
    if (!g_cdo) g_cdo = R::FindClassDefaultObject(L"GameplayStatics");
    if (!g_makeFn) {
        if (void* cls = R::FindClass(L"GameplayStatics")) g_makeFn = R::FindFunction(cls, L"MakeHitResult");
    }
    if (!g_hitStruct && g_makeFn) g_hitStruct = R::PropertyInnerStruct(g_makeFn, L"ReturnValue");
    if (!g_cdo || !g_makeFn || !g_hitStruct || !frame.valid()) {
        UE_LOGW("hit_result: not built (GameplayStatics CDO=%d MakeHitResult=%d HitResult struct=%d frame=%d)",
                g_cdo ? 1 : 0, g_makeFn ? 1 : 0, g_hitStruct ? 1 : 0, frame.valid() ? 1 : 0);
        return false;
    }
    const int32_t size = R::FindParamSize(g_makeFn, L"ReturnValue");
    const int32_t into = R::FindParamSize(frame.function(), param);
    if (size <= 0 || size != into) {
        UE_LOGW("hit_result: not built: MakeHitResult returns %d bytes and '%ls' takes %d", size, param, into);
        return false;
    }
    ParamFrame f(g_makeFn);
    if (!f.valid() || !f.Set<bool>(L"bBlockingHit", true) || !f.Set<void*>(L"HitActor", actor) ||
        !f.Set<void*>(L"HitComponent", component))
        return false;  // ParamFrame logs a name it does not know
    f.Set<FVector>(L"Location", location);
    f.Set<FVector>(L"ImpactPoint", location);
    if (!Call(g_cdo, f)) {
        UE_LOGW("hit_result: not built: the MakeHitResult call faulted");
        return false;
    }
    // Weak pointers and names only: the bytes are the whole value, with nothing to release.
    std::vector<uint8_t> hit(static_cast<size_t>(size));
    if (!f.GetRaw(L"ReturnValue", hit.data(), size)) return false;
    // A hit that does not name what it was built on would send the call somewhere else.
    if (!Names(hit.data(), L"Component", component) || !Names(hit.data(), L"Actor", actor)) {
        UE_LOGW("hit_result: not built: the hit does not resolve back (component=%d actor=%d)",
                Names(hit.data(), L"Component", component) ? 1 : 0, Names(hit.data(), L"Actor", actor) ? 1 : 0);
        return false;
    }
    return frame.SetRaw(param, hit.data(), size);
}

}  // namespace ue_wrap::hit_result
