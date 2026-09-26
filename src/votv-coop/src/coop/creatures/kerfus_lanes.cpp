// coop/creatures/kerfus_lanes.cpp -- see coop/creatures/kerfus_lanes.h.

#include "coop/creatures/kerfus_lanes.h"

#include "coop/creatures/kerfus_brain.h"
#include "coop/creatures/kerfus_follow.h"
#include "coop/creatures/kerfus_intent.h"
#include "coop/creatures/kerfus_state.h"
#include "coop/net/session.h"

namespace coop::kerfus_lanes {

void Install(coop::net::Session& session) {
    coop::kerfus_brain::Install(&session);
    coop::kerfus_state::Install(&session);
    coop::kerfus_intent::Install(&session);
    coop::kerfus_follow::Install(&session);
}

void Tick(coop::net::Session& session) {
    coop::kerfus_brain::Tick();
    coop::kerfus_state::Tick();
    coop::kerfus_intent::Tick(session);
    coop::kerfus_follow::Tick();
}

void OnPeerWorldReady(int slot) { coop::kerfus_state::OnPeerWorldReady(slot); }

void OnPeerLeft(uint8_t slot) {
    coop::kerfus_intent::OnPeerLeft(slot);
    coop::kerfus_follow::OnPeerLeft(slot);
}

void OnDisconnect() {
    coop::kerfus_brain::OnDisconnect();
    coop::kerfus_state::OnDisconnect();
    coop::kerfus_intent::OnDisconnect();
    coop::kerfus_follow::OnDisconnect();
}

}  // namespace coop::kerfus_lanes
