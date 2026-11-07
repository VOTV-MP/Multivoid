// ue_wrap/desk/desk_short.cpp -- see ue_wrap/desk/desk_short.h.

#include "ue_wrap/desk/desk_short.h"

#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

namespace ue_wrap::desk_short {
namespace {

namespace R = ue_wrap::reflection;

// The flag's byte and mask on the desk's class, resolved off the first desk read.
void* g_cls = nullptr;
int32_t g_byte = -1;
uint8_t g_mask = 0;

bool Resolve(void* desk) {
    void* const cls = R::ClassOf(desk);
    if (!cls) return false;
    if (cls == g_cls) return g_byte >= 0;
    g_cls = cls;
    g_byte = -1;
    int32_t byte = -1;
    uint8_t mask = 0;
    if (R::FindBoolProperty(cls, L"shortCircuiting", byte, mask)) {
        g_byte = byte;
        g_mask = mask;
    }
    return g_byte >= 0;
}

}  // namespace

bool ReadShortCircuiting(void* desk, bool& out) {
    if (!desk || !R::IsLive(desk) || !Resolve(desk)) return false;
    out = (static_cast<const uint8_t*>(desk)[g_byte] & g_mask) != 0;
    return true;
}

bool CallShortCircuit(void* desk) {
    if (!desk || !R::IsLive(desk)) return false;
    return ue_wrap::component_calls::CallParamlessNamed(desk, L"shortCircuit");
}

}  // namespace ue_wrap::desk_short
