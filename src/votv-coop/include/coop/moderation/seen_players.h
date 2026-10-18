// coop/moderation/seen_players.h -- HOST-side persistent registry of every player this host has
// ever seen: the durable player id (the proved id, the ban key), the last known nick, the last seen
// time and the last known enforceable address (EnforceableAddress; kept across a join without one).
//
// Gameplay/network layer (principle 7). It feeds the F1 > Administration > Players panel -- the
// offline section shows nick and last seen, and the Ban button needs the record's id, which is the
// ban key (the address only rides along when the admin asks). MTA's precedent is CBanManager
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CBanManager.cpp): persistent, file-backed
// identity records with nick and timestamps.
//
// Persistence is multivoid-players.txt in the mod's data folder, one record per line as
// `guid|nick|lastSeenUnix|ip`. Only the HOST writes it, every touch point being a host-role path,
// and each game copy keeps its own file. The record map is mutex-guarded: GetSnapshot and
// FindByGuid are any-thread (the F1 panel renders on the render thread), while TouchOnJoin is GAME
// THREAD because it reads player_handshake's game-thread strings.

#pragma once

#include "coop/text/utf8_codec.h"  // kNickBufBytes

#include <vector>

namespace coop::net { class Session; }

namespace coop::seen_players {

// Plain-data record for UI consumption (render thread reads copies).
struct Entry {
    char      guid[33] = {};   // the proved player id (the ban key): 32 hex chars + NUL
    char      nick[coop::text::kNickBufBytes] = {};  // last known nick, UTF-8
    char      ip[64]   = {};   // last enforceable remote address (no port); kept across a join without one
    long long lastSeenUnix = 0;
    bool      online = false;  // currently connected to this host's session
};

// Load the registry file into memory. Called once at each host session start, next to
// ban_list::Load.
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

// Look up one record by GUID (for moderation::BanOffline, the offline ban's reply and the
// dispatcher's known-id check (command_sync)). Any thread.
// Returns false if unknown.
bool FindByGuid(const char* guid, Entry& out);

}  // namespace coop::seen_players
