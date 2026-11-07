// coop/creatures/walker_lanes.cpp -- see coop/creatures/walker_lanes.h.

#include "coop/creatures/walker_lanes.h"

#include "coop/creatures/walker_brain.h"
#include "coop/creatures/walker_gaze.h"
#include "coop/creatures/walker_gaze_host.h"
#include "coop/creatures/walker_state_client.h"
#include "coop/creatures/walker_state_host.h"
#include "coop/creatures/walker_target.h"
#include "coop/dev/perf_probe.h"
#include "coop/net/session.h"
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::walker_lanes {

void Install(coop::net::Session& session) {
    coop::walker_brain::Install(session);  // a client's walker copy never thinks: the graph watch and its nav pawn made inert, from the load window on
    coop::walker_target::Install(session);  // the host: the subscription to the prop lane's eid mints, so a walker is claimed into the drive stream
    coop::walker_state_host::Install(session);  // the host: the walker's pose edge and its hunt go to the clients (the damping seam arms with the first held walker)
    coop::walker_gaze_host::Install(session);  // the host: the walker's gaze reads answered with its client target's level (the gaze seams arm with the first client target)
    coop::walker_gaze::Install(session);  // a client: its gaze on the walker that hunts it, sent as WalkerSeen
}

void Tick(coop::net::Session& session, bool isHost) {
    namespace PP = coop::dev::perf_probe;
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:walker_target"}; coop::walker_target::Tick(session); }  // HOST: the walkers born since the last tick are claimed into the drive stream (before the drive pump; an empty queue costs one atomic read)
    if (session.role() == coop::net::Role::Client) coop::walker_state_client::Tick();  // CLIENT: the walker copy's end of play is heard once the class resolves (one comparison after)
    if (session.role() == coop::net::Role::Client) { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:walker_gaze"}; coop::walker_gaze::Tick(); }  // CLIENT: the gaze on each copy that hunts this player, judged each frame, sent on a change (one comparison while none does)
}

void OnPeerWorldReady(int slot) {
    coop::walker_state_host::SendJoinStateToSlot(slot);  // every held walker's pose and the player it hunts, to the joiner alone: its copy holds the state until it is born
}

void OnPeerLeft(uint8_t slot) {
    coop::walker_target::OnPeerLeft(slot);  // a walker that hunted the leaver chooses again at its next read
    coop::walker_gaze_host::OnPeerLeft(slot);  // and the level the leaver sent for any walker
}

void OnDisconnect() {
    coop::walker_brain::OnDisconnect();  // what the walker's graph watch refused, and the spawn hook disarmed
    coop::walker_target::OnDisconnect();  // the claim queue, the held walkers, the hunt hook and the walker class
    coop::walker_state_host::OnDisconnect();  // the session the walker states went through, and the damping seam disarmed
    coop::walker_state_client::OnDisconnect();  // the walker states held for copies
    coop::walker_gaze_host::OnDisconnect();  // the levels clients sent, the refusals counted, and the gaze seams disarmed
    coop::walker_gaze::OnDisconnect();  // the copies that hunted this player
}

}  // namespace coop::walker_lanes
