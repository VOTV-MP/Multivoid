// coop/interactables/laptop_tab.h -- every peer sees the tab the base computer's user is on.
//
// The base laptop and every portable PC show one ui_laptop_C widget, whose pages are a UWidgetSwitcher. The peer that
// holds the "laptop" device claim (coop/interactables/device_occupancy) is the PC's user; its switcher index is
// reported at the POST of the widget's switcherSwitched and of its floppy and sub-game tab clicks, the three bodies
// that move it. A client sends LaptopTabReport and the host, as holder, sends LaptopTabState itself. The host judges a
// report (the sender must hold the claim, the index must be a tab of its own switcher), writes it to its own widget,
// whose index is the canonical, and sends the state to every client but the sender. A peer that does not hold the
// claim shows the tab by writing the switcher's index and nothing else, never switcherSwitched, whose createRC binds a
// viewer's keys to a remote drone or kerfur. "Holds the claim" is the host's table as this peer mirrors it (HolderOf
// equal to its own valid slot): a client that entered before the host's verdict applies the one index it is sent. The
// page content, the cursor and a portable PC's placement stay each peer's own. MTA precedent: element data with a
// client author, checked, set and relayed to all but the source
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2743-2776). Late join: the host's own widget index,
// sent to the joiner at its world-ready.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct LaptopTabPayload;
}  // namespace coop::net

namespace coop::laptop_tab {

// Registers the three name watches and keeps the session for the receive paths. Called every pump tick (the net
// pump's one-shot installs are idempotent) and latched per watch: a watch registers once, and one the gate refused
// stays refused for the process, said once; a later call is three compares. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watches, says the refusals of the window, and pays what is owed. A client pays its newest
// unsent index at the budget's pace while it still holds the claim, and at once when its player leaves the PC; the host
// pays each sender's newest report likewise, and applies the pending report of a sender that left. Game thread, once
// per pump tick, BEFORE device_occupancy::Tick, so the last tab of a leaving user goes ahead of the claim's release.
void Tick(coop::net::Session& session);

// Host: a client's report from the wire, its length already checked by the dispatcher. Game thread.
void OnReport(coop::net::Session& session, const coop::net::LaptopTabPayload& payload, uint8_t senderSlot);

// Client: the host's state from the wire, its length already checked by the dispatcher. Game thread.
void OnState(coop::net::Session& session, const coop::net::LaptopTabPayload& payload, uint8_t senderSlot);

// Host: the joiner's seed, the host's own widget index. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Why a report or a state was refused, counted apiece so a drill proves the cause it claims.
//   NotHolder -- a report from a peer that does not hold the PC (host); a state shown to the peer the claim names (client).
//   BadIndex  -- an index past the last tab of this peer's switcher.
//   BadSlot   -- a state from a slot other than the host's (client).
//   NoWidget  -- this peer has no widget, or no switcher that takes the write, to show or judge the tab on.
enum class Cause : uint8_t { NotHolder, BadIndex, BadSlot, NoWidget };

// How many times this peer has refused for `cause` since the process began (a client's NoWidget counts the states it
// could not show, though it charges no sender). For the drills. Game thread.
uint64_t RefusedCount(Cause cause);

// How many reports (host) or states (client) have reached this module since the process began, whatever became of
// them, and the index the last one carried (-1: none). For the drills, which judge on arrival and not on a clock.
uint64_t ReceivedCount();
int      LastReceivedIndex();

// The session ended: the last word reported, the debt a client owed, the refusal window. A sender's pending report goes
// with its occupant.
void OnDisconnect();

}  // namespace coop::laptop_tab
