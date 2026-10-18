// coop/server_profile/server_profile.h -- the server a host runs, as a folder in the mod's own
// data folder.
//
// A server is one folder, `<mod data dir>\multivoid_servers\<server id>\`, owned by the host: the stores
// a server owns (its settings, permissions, bans, players' profiles) will live under it. MTA's
// precedent is the one-folder layout of a server's files (mtaserver.conf beside acl.xml and
// banlist.xml); keeping each server's stores in its own folder inside one install is our choice,
// MTA keeps one set per install. The id rule is ours: a name a player types is never a path.
//
// This module only names the folder and creates it at a host session start; the stores read and
// write it from the folder it hands back (bans.json is the first). The id of the server this install hosts is kept in multivoid.ini as `net.server`, set
// from the host's nickname at the first host start.

#pragma once

#include <string>
#include <string_view>

namespace coop::server_profile {

// `<mod data dir>\multivoid_servers` (ue_wrap::paths::ModDataDir); empty when the mod has no data
// folder. Creates nothing. Any thread.
std::wstring ServersDir();

// True iff `id` is a usable folder name: 1..24 bytes of [a-z0-9-], not starting or ending with
// '-', and not a Windows device name (con, prn, aux, nul, com1..com9, lpt1..lpt9). Pure.
bool IsValidId(std::string_view id);

// A new server's id from its name: A-Z lowered, a-z and 0-9 kept, every maximal run of any other
// byte (non-ASCII included) becomes one '-', the ends are trimmed, the result is cut to 24 bytes
// and a trailing '-' the cut leaves is removed; empty gives "server" and a device name gets
// "-server" appended. Always satisfies IsValidId. Pure.
std::string IdFromName(std::string_view nameUtf8);

// WHICH server this install hosts, without making it real.
//   rowText    the value of the `net.server` row;
//   id         the server's id: IdFromName(host nickname) when the row is empty (fromNick), the row
//              itself when it is a valid id, IdFromName(rowText) when it is not (handTyped).
// Creates and writes nothing; any thread; COLD (a row read may read the ini from disk).
struct HostingId {
    std::string id;
    bool fromNick = false;
    bool handTyped = false;
    std::string rowText;
};
HostingId IdForHosting(std::string_view hostNickUtf8);

// Once per HOST session start, on the TimelineThread, before the config's session layer is
// filled; never on a client. Resolves the hosted server (setting `net.server` from the nickname at
// the first host start) and creates its folder when absent. RETURNS the folder it created or
// adopted, empty when it could not be made or the config refused the server row: the host session
// continues and the failure is logged. A store takes this value at its load and keeps it (the ban
// list's Load); nothing latches it here.
std::wstring EnsureHosted(std::string_view hostNickUtf8);

// The id rule's selftest, run at each session start; true when every case passes.
bool RunSelftest();

}  // namespace coop::server_profile
