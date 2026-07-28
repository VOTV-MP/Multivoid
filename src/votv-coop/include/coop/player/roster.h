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

#include "coop/net/link_kind.h"
#include "coop/player/players_registry.h"  // kMaxPeers

namespace coop::net { class Session; }

namespace coop::roster {

// "this row has no ID to show" -- the out-of-session synthetic row, which is not
// in any session and therefore has no host-issued number.
inline constexpr unsigned short kNoPlayerNo = 0;

// One roster entry. Plain data only (the render thread reads it) -- the nickname
// is a fixed UTF-8 buffer (peer nicks are sanitized to ASCII upstream, so 23
// bytes + NUL is ample).
struct Row {
    int  slot = -1;
    char nick[24] = {};
    // The occupant's session ID -- the number TAB shows, minted by the host and
    // never reused within a session. 0 only out of session (see kNoPlayerNo).
    // Deliberately NOT the slot: slots recycle, IDs do not.
    unsigned short playerNo = 0;
    // The occupancy generation this row was born from (HOST-side; 0 on a client
    // and out of session). Never displayed and never sent -- it exists so a
    // destructive action captured off this snapshot can be validated against the
    // live net-layer authority at execution time. See coop::moderation::PlayerToken.
    unsigned int   generation = 0;
    bool isLocal = false;    // this row is YOU
    bool isHost  = false;    // this row's peer is the host (slot 0)
    bool connected = false;
    // BOTH connection facts are the HOST's measurement, republished on RosterRow
    // and read straight out of the ledger -- identical on every board, for every
    // row, with no role branching (v131). They answer ONE question: "how is THIS
    // PLAYER connected to the session". Before v131 they answered "how do I
    // reach them", which is a different question per viewer.
    int  ping = -1;   // RTT ms to the SESSION (-1 = not sampled / not applicable, 0 = sub-ms)
    coop::net::LinkKind linkKind = coop::net::LinkKind::Unknown;
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
