// coop/net/player_limit.h -- how many players this host's session seats: the net.max_players row,
// mirrored for the threads that cannot read a row.
//
// A row read opens the ini under a lock, so the net thread (which admits a peer) and the announcer's
// threads (which tell the master) never resolve it; they read this atomic. The game thread sets it
// from the row at boot and again at every change of the row's value: a set, a reset, the session
// layer's fill at a host's session start and its emptying at the end.
//
// The shape is MTA's: a hard ceiling fixed at build time (kMaxPeers) and the host's soft limit under it
// (CMainConfig.cpp:1062, GetMaxPlayers is the smaller of the two); the soft one only decides who may
// join next and never removes anyone.

#pragma once

namespace coop::net::player_limit {

// Subscribe to the row and read it once. Game thread, once, at boot, before any session starts.
void Init();

// The seats the host's session offers, the host's own among them: one of the row's choices, never
// above the ceiling (coop::players::kMaxPeers). Any thread. Before Init it is the row's default.
int Max();

}  // namespace coop::net::player_limit
