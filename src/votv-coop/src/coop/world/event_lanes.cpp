// coop/world/event_lanes.cpp -- see coop/world/event_lanes.h.

#include "coop/world/event_lanes.h"

#include "coop/creatures/boar_sync.h"
#include "coop/dev/perf_probe.h"
#include "coop/net/session.h"
#include "coop/world/agrav_sync.h"
#include "coop/world/copy_graph_gate.h"
#include "coop/world/obelisk_copy.h"
#include "coop/world/trash_fling_sync.h"
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::event_lanes {

void Install(coop::net::Session& session) {
    coop::trash_fling_sync::Install(&session);  // the trash prank's fling: a post watch on the trasher
}

void Tick(coop::net::Session& session, bool isHost) {
    if (!isHost) return;
    namespace PP = coop::dev::perf_probe;
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:trash_fling"}; coop::trash_fling_sync::Tick(&session); }  // the props the trash prank flung
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:agrav"}; coop::agrav_sync::Tick(&session); }  // the props the anti-gravity event lifts
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:boar"}; coop::boar_sync::Tick(&session); }  // the gray boars the gamemode lists
}

void OnDisconnect() {
    coop::agrav_sync::OnDisconnect();  // the lifted props it held
    coop::trash_fling_sync::OnDisconnect();  // the fling it had noted
    coop::boar_sync::OnDisconnect();  // the counts it logged
    coop::copy_graph_gate::OnDisconnect();  // what a client's copies were refused
    coop::obelisk_copy::OnDisconnect();  // the spawn hook disarmed
}

}  // namespace coop::event_lanes
