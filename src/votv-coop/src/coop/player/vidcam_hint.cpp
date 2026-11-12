// coop/player/vidcam_hint.cpp -- see coop/player/vidcam_hint.h.

#include "coop/player/vidcam_hint.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace coop::vidcam_hint {
namespace {

namespace VC = ue_wrap::video_camera;

std::atomic<uint8_t> g_view{static_cast<uint8_t>(VC::View::None)};

}  // namespace

void Update() {
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 250;
    VC::View v = VC::View::None;
    if (!VC::ReadView(v)) v = VC::View::None;
    g_view.store(static_cast<uint8_t>(v), std::memory_order_release);
}

VC::View Current() { return static_cast<VC::View>(g_view.load(std::memory_order_acquire)); }

}  // namespace coop::vidcam_hint
