// coop/player/vidcam_hint.cpp -- see coop/player/vidcam_hint.h.
//
// The overlay's Visibility is written in one place, RCM_cameraManager_C::updUI, which sets it from "recording or
// the camera held" (bytecode ground truth, 0.9.0n); the manager calls updUI when a recording begins, pauses,
// resumes or finishes and after checkCamera's drop or put-away branch. So the watch on updUI's end is the whole
// announcement, and nothing is polled.

#include "coop/player/vidcam_hint.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::vidcam_hint {
namespace {

namespace VC = ue_wrap::video_camera;
namespace sg = ue_wrap::script_gate;

// A name watch: the manager's class is a new object with each level.
UE_FACT_FN(kRowUpdUI, "vidcam_hint", L"RCM_cameraManager_C", L"updUI", Declared);
constexpr int kTag = 0x56434855;  // 'VCHU'

std::atomic<uint8_t> g_view{static_cast<uint8_t>(VC::View::None)};
std::atomic<uint32_t> g_world{0};  // the world generation g_view was read in; 0 = never
bool g_watching = false;           // game thread

void Publish() {
    ue_wrap::world_identity::CurrentWorld();  // the game thread's refresh, so the generation stamped is this world's
    VC::View v = VC::View::None;
    if (!VC::ReadView(v)) v = VC::View::None;
    g_view.store(static_cast<uint8_t>(v), std::memory_order_relaxed);
    g_world.store(ue_wrap::world_identity::Generation(), std::memory_order_release);
}

void OnUpdUIPost(const sg::Call&) { Publish(); }

}  // namespace

void Install() {
    // Drill red (vidcam_hint_red, off by default): no watch, so nothing is ever published.
    static const bool kRed = coop::config::ResolveFlag(coop::config_registry::rows::vidcam_hint_red);
    if (kRed || g_watching || !sg::IsInstalled()) return;
    if (!sg::WatchName(kRowUpdUI, kTag, nullptr, &OnUpdUIPost)) return;
    g_watching = true;
    UE_LOGI("vidcam_hint: watching RCM_cameraManager_C::updUI -- the video camera's overlay state is read when the "
            "game changes it");
    Publish();  // an overlay shown before the watch stood
}

VC::View Current() {
    // A state read in another world is not this one's: a world change announces itself by its generation, and a new
    // world's manager says its own state at its first updUI.
    if (g_world.load(std::memory_order_acquire) != ue_wrap::world_identity::Generation()) return VC::View::None;
    return static_cast<VC::View>(g_view.load(std::memory_order_relaxed));
}

}  // namespace coop::vidcam_hint
