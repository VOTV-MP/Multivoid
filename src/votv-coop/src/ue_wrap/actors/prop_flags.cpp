// ue_wrap/actors/prop_flags.cpp -- see ue_wrap/actors/prop_flags.h.

#include "ue_wrap/actors/prop_flags.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::prop {
namespace {

namespace P = profile;
namespace R = reflection;

template <typename T>
inline T ReadField(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

}  // namespace

bool IsHeavy(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_propData_heavy);
}

bool IsStatic(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_Static);
}

bool IsFrozen(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_frozen);
}

void WriteStatic(void* prop, bool on) {
    if (!prop) return;
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(prop) + P::off::Aprop_Static) = on;
}

void WriteFrozen(void* prop, bool on) {
    if (!prop) return;
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(prop) + P::off::Aprop_frozen) = on;
}

bool CallAwakeUnfreeze(void* prop) {
    if (!prop) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"awakeUnfreeze");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(prop, f);
}

bool IsSleeping(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_sleep);
}

bool ReadRemoveWOrespawn(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_removeWOrespawn);
}

}  // namespace ue_wrap::prop
