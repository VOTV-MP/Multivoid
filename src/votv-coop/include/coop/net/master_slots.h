// coop/net/master_slots.h -- which master servers this player can talk to, and which one they
// chose. A slot is a label and a master address. The list is the `net.masters` row, whose default
// `default` stands for the official masters (protocol.h's kOfficialMasterSlots); the choice is the
// `net.master` row, written when the player picks a tab in the server browser. Every master contact
// that is not about an existing lobby reads the chosen slot here -- the list fetch, a new host's
// announce, the update check, the thanks list -- and a join goes through the master its lobby was
// listed on, which the row carries. Our masters are independent shards (a lobby, its rendezvous and
// its relay live on one of them), so one list is shown at a time and only the chosen master learns
// the player's address. MTA divergence: its client merges replicas of one list
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

// Parses a list: comma-separated `label=address` entries, the first being the default. The entry
// `default` stands for the official masters, so a list that adds its own keeps whatever a later
// build ships. A bare address is a slot labelled with its host, so a single URL is a list of one;
// a path after the port is dropped, as the HTTP client drops it. An entry that is malformed, past
// the bounds or repeats a label is skipped and described in `rejected`. Pure.
std::vector<Slot> Parse(const std::string& text, std::vector<std::string>* rejected);

// Reads the two rows and settles the selection, logging the list once. Idempotent, and every
// accessor below runs it on first use, so no reader depends on boot order. A list from the
// environment that names no usable master is NOT replaced by the official one: a test's client
// must never reach production unasked, so every contact then fails where it is made.
void Init();

std::vector<Slot> List();
int  SelectedIndex();
Slot Selected();

// The player chose slot `index`: it becomes current and, when the list is the player's own (not a
// test's from the environment), its label is written to the ini -- file I/O, so a click's cost, not
// a tick's. False for an index out of range.
bool Select(int index);

// Whether Select writes the choice to the ini: false while the list comes from the environment.
bool ChoiceIsRemembered();

// How a line that names a master or an endpoint on a master's host prints it: the slot's label, so
// such a line does not spell out an address the player did not type; anything else as written.
std::string DisplayName(const std::string& endpoint);

// The signaling relay on the chosen master's host, for a P2P session dialled with no master in
// the loop (a test, or a `gen:` line); empty when no master is chosen. A lobby's own rendezvous
// comes from its master's answer.
std::string DefaultSignalingUrl();

}  // namespace coop::net::master_slots
