// coop/dev/host_settle_drill.cpp -- see coop/dev/host_settle_drill.h.

#include "coop/dev/host_settle_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/props/host_spawn_watcher.h"
#include "coop/session/host_world_settle.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop_processor.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstddef>

namespace coop::dev::host_settle_drill {
namespace {

namespace CI = ue_wrap::container_inventory;

constexpr size_t kContainerCap = coop::host_world_settle::kContainerCap;
void* g_buf[kContainerCap];

unsigned g_held = 0;        // requests that waited because the world had not settled
bool     g_captured = false;
bool     g_failed = false;  // any FAIL line printed
bool     g_done = false;

// The containers the gamemode lists that have not initialized their inventory.
size_t Uninitialized() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return 0;
    const size_t n = CI::AllContainers(gm, g_buf, kContainerCap);
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i)
        if (!CI::InventoryInitialized(g_buf[i])) ++bad;
    return bad;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::host_settle_drill);
    return s;
}

void NoteHold() {
    if (!IsEnabled()) return;
    if (ue_wrap::world_singleton::Gamemode() && !coop::host_world_settle::Settled()) ++g_held;
}

void NoteCapture() {
    if (!IsEnabled()) return;
    g_captured = true;
    if (coop::host_world_settle::TrueSettledNow()) return;
    UE_LOGI("[HOST-SETTLE-DRILL] host FAIL: the capture ran before the host's world settled (prop passes %d, "
            "%zu containers uninitialized)",
            ue_wrap::prop_processor::Passes(ue_wrap::prop_processor::WorldPropProcessor()), Uninitialized());
    g_failed = true;
}

void Tick(coop::net::Session& session) {
    if (!IsEnabled() || g_done || !g_captured) return;
    if (session.role() != coop::net::Role::Host) return;
    int slot = -1;
    for (int i = 1; i < coop::net::kMaxPeers && slot < 0; ++i)
        if (session.IsSlotWorldReady(i)) slot = i;
    if (slot < 0) return;
    g_done = true;
    const size_t drops = coop::host_spawn_watcher::PendingCapDrops();
    if (drops > 0) {
        g_failed = true;
        UE_LOGI("[HOST-SETTLE-DRILL] host FAIL: %zu births dropped at the pending cap", drops);
    }
    if (!g_failed) {
        if (g_held == 0)
            UE_LOGI("[HOST-SETTLE-DRILL] host INCONCLUSIVE: the join never met the host's start-up");
        else
            UE_LOGI("[HOST-SETTLE-DRILL] host PASS slot %d", slot);
    }
    UE_LOGI("[HOST-SETTLE-DRILL] host DONE");
}

}  // namespace coop::dev::host_settle_drill
