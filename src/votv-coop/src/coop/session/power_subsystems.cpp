// coop/session/power_subsystems.cpp -- see coop/session/power_subsystems.h.

#include "coop/session/power_subsystems.h"

#include "coop/world/fuse_tower_ops.h"
#include "coop/world/fuse_tower_rows.h"
#include "coop/world/power_grid.h"
#include "coop/world/power_panel.h"
#include "coop/dev/perf_probe.h"
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::subsystems::power {

void Install(coop::net::Session& session) {
    coop::power_panel::Install(&session);  // the power panel: presses to the host, its canonical to every peer
    coop::power_grid::Install(&session);  // the grid runs on the host: its generators' rows, a client's ops
    coop::fuse_tower_rows::Install(&session);  // the towers roll and judge on the host: their rows to every peer
    coop::fuse_tower_ops::Install(&session);  // a client's presses and fuse claims, and its refused rolls
}

void OnPeerLeft(uint8_t slot) {
    coop::power_panel::OnPeerLeft(slot);  // and its presses that wait for its body
    coop::power_grid::OnPeerLeft(slot);  // and its generator ops
    coop::fuse_tower_ops::OnPeerLeft(slot);  // and its tower ops
}

void Tick() {
    namespace PP = coop::dev::perf_probe;
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:power"}; coop::power_panel::Tick(); coop::power_grid::Tick(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:towers"}; coop::fuse_tower_ops::Tick(); coop::fuse_tower_rows::Tick(); }  // the towers: the gates settle, the host's ops and rows, a client's panels  // the panel's and the grid's gates settle; what came before the panel or the generators resolved is taken
}

void OnDisconnect() {
    coop::power_panel::OnDisconnect();
    coop::power_grid::OnDisconnect();
    coop::fuse_tower_ops::OnDisconnect();
    coop::fuse_tower_rows::OnDisconnect();
}

}  // namespace coop::subsystems::power
