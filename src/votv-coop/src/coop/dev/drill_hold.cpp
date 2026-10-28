// coop/dev/drill_hold.cpp -- see coop/dev/drill_hold.h.

#include "coop/dev/drill_hold.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

namespace coop::dev::drill_hold {
namespace {

namespace R = ue_wrap::reflection;

// A reflected bool's byte and mask on `obj`; null when the object or the member does not resolve.
uint8_t* BoolAt(void* obj, const wchar_t* name, uint8_t& mask) {
    int32_t off = -1;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return nullptr;
    return static_cast<uint8_t*>(obj) + off;
}

}  // namespace

bool Hold(void* obj, const wchar_t* name, int& held, int restoreTo, const char* tag) {
    uint8_t mask = 0;
    uint8_t* b = R::IsLive(obj) ? BoolAt(obj, name, mask) : nullptr;
    if (!b) return false;
    const bool now = (*b & mask) != 0;
    if (held < 0) held = restoreTo >= 0 ? restoreTo : (now ? 1 : 0);
    if (now) return true;
    *b = static_cast<uint8_t>(*b | mask);
    UE_LOGI("%s guard %ls set to 1", tag, name);
    return true;
}

void Put(void* obj, const wchar_t* name, int& held, const char* tag) {
    if (held < 0) return;
    uint8_t mask = 0;
    uint8_t* b = R::IsLive(obj) ? BoolAt(obj, name, mask) : nullptr;
    if (b) *b = held != 0 ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    UE_LOGI("%s guard %ls restored to %d%s", tag, name, held, b ? "" : " (object gone)");
    held = -1;
}

}  // namespace coop::dev::drill_hold
