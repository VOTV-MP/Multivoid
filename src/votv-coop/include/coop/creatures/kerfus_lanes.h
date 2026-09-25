// coop/creatures/kerfus_lanes.h -- the plain kerfur's (the Kerfus, p_kerfus_C) three lanes, wired in
// one place: its brain refused on a client (kerfus_brain), its state and drive from the host
// (kerfus_state), a client's verbs as the host's (kerfus_intent). Each registers its watches (Install,
// a per-tick retry), settles them and runs its queue or its waiting states (Tick), and drops its
// session state (OnDisconnect); a joiner's world-ready gets every state again and a leaver's intents
// go. This calls them, in that order. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::kerfus_lanes {

void Install(coop::net::Session& session);
void Tick(coop::net::Session& session);
void OnPeerWorldReady(int slot);
void OnPeerLeft(uint8_t slot);
void OnDisconnect();

}  // namespace coop::kerfus_lanes
