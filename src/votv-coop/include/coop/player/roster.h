// coop/player/roster.h -- thread-safe player-roster snapshot for the player-list scoreboard.
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
#include "coop/text/utf8_codec.h"          // kNickBufBytes -- the nick buffer's ONE owner

namespace coop::net { class Session; }

namespace coop::roster {

// "this row has no ID to show" -- the out-of-session synthetic row, which is not
// in any session and therefore has no host-issued number.
inline constexpr unsigned short kNoPlayerNo = 0;

// One roster entry. Plain data only (the render thread reads it) -- the nickname
// is a fixed UTF-8 buffer sized by the display policy itself
// (coop::text::kNickBufBytes), not by an ASCII assumption. The old 24-byte
// literal predated arc D1's widened alphabet: it fit 23 ASCII characters but only
// 11 Cyrillic and 7 hanzi, and the row went BLANK past that.
struct Row {
    int  slot = -1;
    char nick[coop::text::kNickBufBytes] = {};
    // The occupant's session ID -- the number TAB shows, minted by the host and
    // never reused within a session. 0 only out of session (see kNoPlayerNo).
    // Deliberately NOT the slot: slots recycle, IDs do not.
    unsigned short playerNo = 0;
    // The occupant's proved player id (32 lower-case hex), the name a moderation command line
    // addresses a person by and the name a permission file is stored under. Filled by the HOST's
    // publisher from the ledger (slot 0: the local id); empty when the proof has not landed, on a
    // client (it never learns other players' ids) and out of session.
    char playerId[33] = {};
    bool isLocal = false;    // this row is YOU
    bool isHost  = false;    // this row's peer is the host (slot 0)
    bool connected = false;
    // Host only: this player was admitted under net.allow_other_builds with another build. False on a
    // client and for the host's own row.
    bool otherBuild = false;
    // BOTH connection facts are the HOST's measurement, republished on RosterRow.
    // They answer ONE question -- "how is THIS PLAYER connected to the session" --
    // rather than "how do I reach them", so no row is special-cased by what the
    // viewer happens to be able to measure. The one substitution is row 0: the host
    // has no link to itself, so a client board shows its own host RTT there.
    // roster_ledger::DisplayLink owns that, in one place.
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

// Lock-free read of "am I the HOST OF A LIVE SESSION?" -- the ROLE question, which every caller
// asks to decide whether a host-only surface or a host-only key binding is theirs. Deliberately
// NOT the snapshot's `localIsHost`, which is true out of session as well (the board shows the row
// you would occupy once you started one): a solo player is not a host. The overlay checks this on
// a hot path (the SetCursorPos detour) to decide capture, so it must not copy the whole snapshot
// under the mutex. Updated by Refresh. Any thread.
bool LocalIsHost();

}  // namespace coop::roster
