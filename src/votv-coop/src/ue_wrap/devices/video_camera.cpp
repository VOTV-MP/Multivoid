// ue_wrap/devices/video_camera.cpp -- see ue_wrap/devices/video_camera.h.

#include "ue_wrap/devices/video_camera.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::video_camera {
namespace {

namespace R = reflection;

// UWidget::Visibility as ESlateVisibility: the manager's updUI writes 4 (SelfHitTestInvisible) to show the
// overlay and 1 (Collapsed) to hide it.
constexpr uint8_t kCollapsed = 1;
constexpr uint8_t kHidden = 2;

// A property's offset on a class, looked up again when the class is a new object (a new world's).
struct Field {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    int32_t On(void* c) {
        if (c != cls) { cls = c; off = c ? R::FindPropertyOffset(c, name) : -1; }
        return off;
    }
};

Field g_manager{L"RCM_manager"};
Field g_camUI{L"camUI"};
Field g_visibility{L"Visibility"};

void* ReadObj(void* obj, Field& f) {
    const int32_t off = obj ? f.On(R::ClassOf(obj)) : -1;
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(obj) + off) : nullptr;
}

}  // namespace

bool ReadView(View& out) {
    out = View::None;
    void* manager = ReadObj(world_singleton::Gamemode(), g_manager);
    if (!manager || !R::IsLive(manager)) return false;
    void* ui = ReadObj(manager, g_camUI);
    const int32_t visOff = ui && R::IsLive(ui) ? g_visibility.On(R::ClassOf(ui)) : -1;
    if (visOff < 0) return false;
    const uint8_t vis = *(static_cast<const uint8_t*>(ui) + visOff);
    if (vis == kCollapsed || vis == kHidden) return true;
    // The native recorder's own answer (BaseRecord::GetIsRecording), true while a recording is paused too.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(manager), L"GetIsRecording");
    ParamFrame pf(fn);
    if (!fn || !pf.valid() || !Call(manager, pf)) return false;
    out = pf.Get<bool>(L"ReturnValue") ? View::Recording : View::Viewfinder;
    return true;
}

}  // namespace ue_wrap::video_camera
