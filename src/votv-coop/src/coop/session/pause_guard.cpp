// coop/pause_guard.cpp -- see coop/pause_guard.h.

#include "coop/session/pause_guard.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"

namespace coop::pause_guard {
namespace {

// Log once per pause EPISODE (a held ESC menu would otherwise spam per tick while
// the un-pause verb is racing the menu's own re-pause). Game-thread only.
bool g_inEpisode = false;

}  // namespace

void Tick(bool connected) {
    if (!connected) { g_inEpisode = false; return; }
    if (!ue_wrap::engine::IsGamePaused()) { g_inEpisode = false; return; }
    if (!g_inEpisode) {
        g_inEpisode = true;
        UE_LOGI("pause_guard: world pause detected in a coop session -- un-pausing (pause is "
                "single-player-only; the ESC menu stays usable, the world keeps ticking)");
    }
    ue_wrap::engine::SetGamePaused(false);
}

}  // namespace coop::pause_guard
