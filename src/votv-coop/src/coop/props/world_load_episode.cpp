// coop/props/world_load_episode.cpp -- see coop/props/world_load_episode.h.
//
// A single game-thread bool. The join-window driver (join_membership_sweep) owns the CLEAR edge (it already
// drives the downstream join modules at load-tail quiescence); this module owns only the ARM (the causal
// world-load trigger, from the harness client-join path) + the query the destroy seam reads.

#include "coop/props/world_load_episode.h"

#include "ue_wrap/log.h"

namespace coop::world_load_episode {
namespace {
// Game-thread only (armed on the join bringup thread's Post to the game thread; cleared on the client
// reconcile tick; read on the destroy seam -- all game thread). No atomic needed.
bool g_inEpisode = false;
}  // namespace

void Arm() {
    if (g_inEpisode) return;  // idempotent -- one arm per world-load
    g_inEpisode = true;
    UE_LOGI("world_load_episode: ARMED -- client world-load starting; KEYED-prop destroy broadcasts "
            "suppressed until load-tail quiescence (host-wipe root fix)");
}

void NotifyQuiesced() {
    if (!g_inEpisode) return;
    g_inEpisode = false;
    UE_LOGI("world_load_episode: CLOSED at load-tail quiescence -- KEYED-prop destroy broadcasts resume");
}

void Reset() { g_inEpisode = false; }

bool InEpisode() { return g_inEpisode; }

}  // namespace coop::world_load_episode
