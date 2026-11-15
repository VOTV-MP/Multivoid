// coop/world/event_lanes.cpp -- see coop/world/event_lanes.h.

#include "coop/world/event_lanes.h"

#include "coop/creatures/boar_sync.h"
#include "coop/dev/perf_probe.h"
#include "coop/net/session.h"
#include "coop/world/agrav_sync.h"
#include "coop/world/copy_graph_gate.h"
#include "coop/world/basalt_pillar.h"
#include "coop/world/easter_hunt.h"
#include "coop/world/evil_ending.h"
#include "coop/world/level_fixture_sync.h"
#include "coop/world/vending_sync.h"
#include "coop/world/door_jam_sync.h"
#include "coop/world/erie_sync.h"
#include "coop/world/event_world_sync.h"
#include "coop/world/obelisk_copy.h"
#include "coop/world/solto_sync.h"
#include "coop/world/trash_fling_sync.h"
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::event_lanes {

void Install(coop::net::Session& session) {
    coop::trash_fling_sync::Install(&session);  // the trash prank's fling: a post watch on the trasher
    coop::evil_ending::Install(&session);  // the Evil's ending, shown to every player
    coop::door_jam_sync::Install(&session);  // the jammed doors
    coop::erie_sync::Install(&session);  // the Erie plush: a client's hit on its copy asked of the host, the morph's achievement sent
    coop::vending_sync::Install(&session);  // the vending machines: a use asked of the host, its stock sent
    coop::level_fixture_sync::Install(&session);  // the church, the booth, the siren pole and the white door, where the host's stand
    coop::easter_hunt::Install(&session);  // the Easter hunt: the well, the bunny's talk and eggs, the host's for everyone
    coop::basalt_pillar::Install(&session);  // the Basalt Pillars: a client's dig asked of the host, each pillar's height sent
    coop::solto_sync::Install(&session);  // the Soltomia craft's start and leave, for a client's copy
    coop::event_world_sync::Install(&session);  // the lake, the ship tower, the radio-tower vanish, the red phone, the locker roll, the meat rain
}

void Tick(coop::net::Session& session, bool isHost) {
    coop::evil_ending::Tick();  // HOST: its Evil's ending seen (one class lookup when none was ever minted); CLIENT: a held cue
    coop::easter_hunt::Tick();  // HOST: the bunny's state twice a second; CLIENT: a state waiting for its copy (returns at once when no bunny stands)
    coop::basalt_pillar::Tick();  // HOST: every two seconds, the pillars that rose; CLIENT: a height waiting for its copy
    coop::door_jam_sync::Tick();  // HOST: a door whose jam changed, once a second; CLIENT: a jam waiting for its door
    coop::erie_sync::Tick();  // HOST: a plush whose paper or pencil changed, once a second; CLIENT: a state waiting for its plush
    coop::vending_sync::Tick();  // HOST: each machine's stock once a second; CLIENT: a stock waiting for its machine
    coop::level_fixture_sync::Tick();  // HOST: a look at its fixtures once a second; CLIENT: a copy waiting to be unseen
    coop::solto_sync::Tick();  // a craft cue that waits for its copy (returns at once when none does)
    coop::event_world_sync::Tick();  // a cue that waits for its target on a client; the host's sound seam and a client's rain refusal armed
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
    coop::evil_ending::OnDisconnect();  // an ending in progress here, and a held cue
    coop::easter_hunt::OnDisconnect();  // the bunny's state sent and waiting
    coop::basalt_pillar::OnDisconnect();  // the heights sent and the ones waiting
    coop::door_jam_sync::OnDisconnect();
    coop::erie_sync::OnDisconnect();
    coop::vending_sync::OnDisconnect();
    coop::level_fixture_sync::OnDisconnect();  // what the host named, and the stand-ins
    coop::solto_sync::OnDisconnect();  // a held craft cue and the sponge hook
    coop::event_world_sync::OnDisconnect();  // the held cues, the announced lockers and the two seams
}

}  // namespace coop::event_lanes
