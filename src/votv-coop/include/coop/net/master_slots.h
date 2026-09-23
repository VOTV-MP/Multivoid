// coop/net/master_slots.h -- which master servers this player can talk to, and which one they
// chose. A slot is a label and a master address. The list is the `net.masters` row, whose default
// is the official masters (protocol.h's kOfficialMasterSlots); the choice is the `net.master` row,
// written when the player picks a tab in the server browser. Every master contact that is not
// about an existing lobby reads the chosen slot here -- the list fetch, a new host's announce, the
// update check, the thanks list -- and a join goes through the master its lobby was listed on,
// which the row carries. Our masters are independent shards (a lobby, its rendezvous and its relay
// live on one of them), so one list is shown at a time and only the chosen master learns the
// player's address. MTA divergence: its client merges replicas of one list
// (CServerBrowser.MasterServerManager.cpp); ours would merge lobbies that live apart. Thread-safe.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace coop::net::master_slots {

struct Slot {
    std::string label;   // what the browser's tab says: ASCII, unique ignoring case
    std::string url;     // the master's address in the HTTP client's grammar ("host:port" is TLS)
};

// Bounds on a list. The picker lays out one tab per slot, so the count bounds its width, and a
// label is one tab's text.
inline constexpr size_t kMaxSlots = 6;
inline constexpr size_t kMaxLabelChars = 16;

// Parses a list: comma-separated `label=address` entries, the first being the default. A bare
// address is a slot labelled with its host, so a single URL is a list of one. An entry that is
// malformed, past the bounds or repeats a label is skipped and described in `rejected`. Pure.
std::vector<Slot> Parse(const std::string& text, std::vector<std::string>* rejected);

// Reads the two rows and settles the selection, logging the list once. Idempotent, and every
// accessor below runs it on first use, so no reader depends on boot order.
void Init();

std::vector<Slot> List();
int  SelectedIndex();
Slot Selected();

// The player chose slot `index`: it becomes current and its label is written to the ini (file
// I/O, so a click's cost, not a tick's). False for an index out of range.
bool Select(int index);

// How a user-visible line names a master or an endpoint on a master's host: the slot's label, so
// the console never prints an address the player did not type; anything else as written.
std::string DisplayName(const std::string& endpoint);

// The signaling relay on the chosen master's host, for a P2P session dialled with no master in
// the loop (a test, or a `gen:` line). A lobby's own rendezvous comes from its master's answer.
std::string DefaultSignalingUrl();

}  // namespace coop::net::master_slots
