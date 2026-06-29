// coop/roster.h -- thread-safe player-roster snapshot for the player-list scoreboard.
//
// Gameplay/network layer (principle 7). The scoreboard renders on the RENDER
// thread but the roster facts live in game-thread-owned state (Session connection
// slots + player_handshake's std::wstring nicknames). Reading those directly off
// the render thread would race the game thread, so this module snapshots them on
// the game thread (Refresh, called from a game-thread tick) into a small POD under
// a mutex; the render thread copies it out via GetSnapshot. No UFunction / UObject
// access here -- pure Session + nickname reads.
//
// Slot model (matches coop::players::Registry): slot 0 = host, 1..kMaxPeers-1 =
// clients. The local peer's slot is its LocalPeerId (host=0).

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

namespace coop::net { class Session; }

namespace coop::roster {

// One roster entry. Plain data only (the render thread reads it) -- the nickname
// is a fixed UTF-8 buffer (peer nicks are sanitized to ASCII upstream, so 23
// bytes + NUL is ample).
struct Row {
    int  slot = -1;
    char nick[24] = {};
    bool isLocal = false;    // this row is YOU
    bool isHost  = false;    // this row's peer is the host (slot 0)
    bool connected = false;
    int  ping = -1;          // RTT ms to this peer (-1 = local/unmeasured, 0 = sub-ms LAN)
    // Connection-type label ("LAN HOST"/"P2P HOST" on the host's own row;
    // "LAN"/"P2P"/"P2P RELAY" for links this peer OWNS; "VIA HOST" for other
    // clients seen from a client board; "" = nothing to show, the own client
    // row -- its link is already on the host row).
    char link[12] = {};
};

struct Snapshot {
    int  count = 0;
    Row  rows[coop::players::kMaxPeers];
    bool localIsHost = false;  // local peer's role is Host (drives the host interactive board)
    bool inSession   = false;  // a Session is running
};

// Cache the Session pointer once at boot. Lock-free.
void SetSession(coop::net::Session* session);

// Rebuild the snapshot from the Session + player_handshake nicks. GAME THREAD only
// (it reads the game-thread-owned nickname strings). Internally throttled (~6 Hz)
// so it's cheap to call from a high-rate tick.
void Refresh();

// Copy the latest snapshot. Safe from ANY thread (the render thread reads it).
void GetSnapshot(Snapshot& out);

// Lock-free read of just "is the local peer the host?" -- the overlay checks this
// on a hot path (the SetCursorPos detour) to decide capture, so it must not copy
// the whole snapshot under the mutex. Updated by Refresh. Any thread.
bool LocalIsHost();

}  // namespace coop::roster
