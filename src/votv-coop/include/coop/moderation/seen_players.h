// coop/moderation/seen_players.h -- HOST-side persistent registry of every player this
// host has ever seen: durable identity GUID (the v73 32-hex Join field) + last known
// nick + last seen time + last known IP.
//
// Gameplay/network layer (principle 7). Feeds the F1 > Administration > Players panel
// (Offline section: nick + last seen; Ban button needs the last IP -- the ban_list
// enforcement key). MTA precedent: the admin resource's seen-players/ban bookkeeping
// around CBanManager (reference/mtasa-blue/Server/mods/deathmatch/logic/CBanManager.cpp)
// -- persistent identity records with nick + timestamps, file-backed.
//
// Persistence: multivoid-players.txt next to the deployed DLL (the ban_list /
// multivoid.ini convention) -- one record per line, `guid|nick|lastSeenUnix|ip`.
// Only the HOST ever writes it (the touch points are host-role paths); each game
// copy keeps its own file.
//
// Threading: the record map is mutex-guarded; GetSnapshot/FindByGuid are any-thread
// (the F1 panel renders on the render thread). TouchOnJoin is GAME THREAD (it reads
// player_handshake's game-thread-owned guid/nick strings).

#pragma once

#include <vector>

namespace coop::net { class Session; }

namespace coop::seen_players {

// Plain-data record for UI consumption (render thread reads copies).
struct Entry {
    char      guid[33] = {};   // 32 hex chars + NUL (validated upstream at the wire)
    char      nick[24] = {};   // last known nick (ASCII-sanitized upstream)
    char      ip[64]   = {};   // last known remote IP (dotted-decimal, no port)
    long long lastSeenUnix = 0;
    bool      online = false;  // currently connected to this host's session
};

// Load the registry file into memory. Called once at boot, next to ban_list::Load.
void Load();

// HOST, game thread: a peer's Join landed for `peerSlot` -- record/update its
// GUID + nick + IP, stamp lastSeen=now, mark online, persist. No-op if the peer
// sent no (valid) GUID -- an unidentifiable peer can't be registered.
void TouchOnJoin(coop::net::Session& session, int peerSlot);

// A slot disconnected: stamp lastSeen=now on its record, clear the online mark,
// persist. No-op for slots never marked online (client role / unknown peers).
void OnSlotDisconnected(int peerSlot);

// Session start: clear every online mark (file-scope state persists across
// Session::Stop()/Start() in one process -- the event_feed reset discipline).
void OnSessionStart();

// Copy all records, most-recently-seen first. Any thread.
void GetSnapshot(std::vector<Entry>& out);

// Look up one record by GUID (for moderation::BanOffline). Any thread.
// Returns false if unknown.
bool FindByGuid(const char* guid, Entry& out);

}  // namespace coop::seen_players
