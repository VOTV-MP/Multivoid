// coop/ban_list.h -- persistent host-side IP banlist (Phase 2 moderation).
//
// MTA precedent: CBanManager / CBan (reference/mtasa-blue/Server/mods/
// deathmatch/logic/CBanManager.{h,cpp} + CBan.h). MTA keys bans by IP +
// serial + nick and persists them to banlist.xml; the join-time check
// (CGame.cpp:1956/1973, inside Packet_PlayerJoinData) rejects a banned
// IP/serial before the player spawns. We follow the same SHAPE, trimmed to
// what our model has: there are no hardware serials in the GNS LAN-direct
// topology, so we key by IP only. The nick + ban time are stored for the
// admin's reference (display / file readability), not for matching.
//
// Persistence: a plain line-based text file (one ban per line:
//   ip|nick|unixtime
// ) under %LOCALAPPDATA%\VotV\Saved\coop\banlist.txt -- the host machine's
// own config area, alongside the save backups save_guard writes. Survives
// host restarts (the locked decision: BAN is permanent). HOST-ONLY -- a
// client never accepts incoming connections, so the banlist is meaningless
// off-host and is simply never loaded there.
//
// Thread model: Load() runs once at host session start (boot thread, before
// the net thread spawns). IsBanned() is read on the NET thread (the GNS
// accept callback's filter). Add() is called on the GAME thread (an admin
// click marshalled via GT::Post in coop::moderation). All three serialize on
// one internal mutex; the in-memory set is the source of truth at runtime and
// the file is rewritten on every Add so a crash can't lose a just-applied ban.

#pragma once

namespace coop::ban_list {

// Host: load the on-disk banlist into memory. Idempotent (re-Load replaces the
// in-memory set). Safe to call when the file doesn't exist yet (empty banlist).
// Call once at host session start, before Session::Start spawns the net thread.
void Load();

// Net thread: is this remote IP banned? `ip` is the dotted-decimal string GNS
// produces from SteamNetConnectionInfo_t::m_addrRemote (port excluded). Cheap
// (one mutex + hash lookup); called once per incoming connection at accept.
bool IsBanned(const char* ip);

// Game thread: ban an IP permanently (adds to the in-memory set AND rewrites the
// file). `nick` is stored for readability only. No-op on empty/null ip. Adding
// an already-banned IP just refreshes its record (idempotent).
void Add(const char* ip, const char* nick);

// Diagnostics: current number of banned IPs.
int Count();

}  // namespace coop::ban_list
