// coop/moderation/ban_list.h -- persistent host-side IP banlist.
//
// MTA precedent: CBanManager / CBan
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CBanManager.{h,cpp} + CBan.h). MTA keys bans
// by IP, serial and nick, persists them to banlist.xml, and rejects a banned one inside
// Packet_PlayerJoinData before the player spawns. We follow the same SHAPE, trimmed to our model:
// GNS LAN-direct carries no hardware serial, so we key by IP alone, and the nick and ban time are
// stored for the admin's reference rather than for matching.
//
// HOST-ONLY: a client never accepts incoming connections, so the banlist is meaningless off-host
// and never loaded there. A ban is permanent and survives host restarts. Persistence is a
// line-based text file, one ban per line as `ip|nick|unixtime|reason`, named multivoid-banlist.txt
// beside the mod binary in Binaries\Win64, resolved through our own module handle, so each game
// copy keeps its own. The in-memory set is the runtime source of truth and the file is rewritten on
// every Add, so a crash cannot lose a just-applied ban.

#pragma once

#include "coop/text/utf8_codec.h"  // kNickBufBytes

#include <vector>

namespace coop::ban_list {

// Plain-data record for UI consumption (the F1 Administration panel's Banned
// section renders copies on the render thread).
struct Entry {
    char      ip[64]     = {};
    char      nick[coop::text::kNickBufBytes] = {};
    char      reason[96] = {};
    long long bannedUnix = 0;
};

// Host: load the on-disk banlist into memory. Idempotent -- a re-Load replaces the set rather than
// merging. Runs once at host session start, on the boot thread, before the net thread spawns.
void Load();

// Net thread: is this remote IP banned? `ip` is the dotted-decimal string GNS produces from
// SteamNetConnectionInfo_t::m_addrRemote (port excluded). Cheap -- one mutex and one hash lookup --
// and called once per incoming connection at accept. Load, this and Add each run on a different
// thread and serialize on that one mutex.
bool IsBanned(const char* ip);

// Game thread: ban an IP permanently (adds to the in-memory set AND rewrites the
// file). `nick` and `reason` are stored for the admin's reference (Banned-section
// rows). No-op on empty/null ip. Adding an already-banned IP just refreshes its
// record (idempotent).
void Add(const char* ip, const char* nick, const char* reason);

// Unban an IP (removes from the in-memory set AND rewrites the file). Returns
// false if the IP wasn't banned. Any thread (internal mutex) -- the F1 panel's
// Unban button calls it directly off the render thread.
bool Remove(const char* ip);

// Copy all ban records, most recent first. Any thread.
void GetSnapshot(std::vector<Entry>& out);

}  // namespace coop::ban_list
