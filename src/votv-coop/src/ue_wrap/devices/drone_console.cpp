// ue_wrap/devices/drone_console.cpp -- see ue_wrap/devices/drone_console.h.

#include "ue_wrap/devices/drone_console.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_instances.h"

#include <cstdint>

namespace ue_wrap::drone_console {

namespace R = ue_wrap::reflection;

namespace {

const wchar_t* const kConsoleClass = L"droneConsole_C";

// The fields the console's own verb reads, resolved on first sight: a class the next world loads again
// has the same layout.
int32_t g_conOpenedOff   = -1;  // droneConsole_C::opened          (the lid)
int32_t g_conKeyboardOff = -1;  // droneConsole_C::lookatKeyboard  (the presser's own cursor)
int32_t g_conDroneOff    = -1;  // droneConsole_C::drone           (a level reference)

// A console field by name, resolved once off the live instance's own class.
bool ConsoleBool(void* console, int32_t& off, const wchar_t* name, bool& out) {
    if (!IsConsole(console)) return false;
    if (off < 0) off = R::FindPropertyOffset(R::ClassOf(console), name);
    if (off < 0) return false;
    out = *reinterpret_cast<bool*>(reinterpret_cast<char*>(console) + off);
    return true;
}

}  // namespace

int32_t LiveConsoles(void** out, int32_t cap) { return world_instances::Find(kConsoleClass, out, cap); }

bool IsConsole(void* obj) {
    void* cls = object_index::ClassByName(kConsoleClass);
    if (!obj || !cls) return false;
    void* objCls = R::ClassOf(obj);
    void* bases[1] = {cls};
    return objCls && R::IsDescendantOfAny(objCls, bases, 1);
}

bool IsLidOpen(void* console) {
    bool open = false;
    return ConsoleBool(console, g_conOpenedOff, L"opened", open) && open;
}

bool IsCursorOnKeyboard(void* console) {
    bool on = false;
    return ConsoleBool(console, g_conKeyboardOff, L"lookatKeyboard", on) && on;
}

bool TriggerFly(void* console) {
    if (!IsConsole(console)) return false;
    if (g_conDroneOff < 0) g_conDroneOff = R::FindPropertyOffset(R::ClassOf(console), L"drone");
    if (g_conDroneOff < 0) return false;
    void* drone = *reinterpret_cast<void**>(reinterpret_cast<char*>(console) + g_conDroneOff);
    if (!drone || !R::IsLive(drone)) return false;
    // Looked up through the cache that holds it by slot and serial, never kept by a bare pointer across
    // worlds (poll arc 2.7's rule for a level class's function).
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(drone), L"triggerFly");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"console", console);
    return ue_wrap::Call(drone, f);
}

}  // namespace ue_wrap::drone_console
