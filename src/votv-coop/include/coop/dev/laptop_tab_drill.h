// coop/dev/laptop_tab_drill.h -- [dev] everyone sees the tab the base computer's user is on: the acceptance arms
// (laptop_tab_drill = tab | tab-host | tab-handover | tab-norc | tab-subgame | tab-range | tab-forged | tab-slot |
// tab-join | tab-relay), set on BOTH peers (tab-relay on the rig's second client too: --observer). The user walks into
// the laptop's Box with the director, turns onto its boxScreen (where the game offers the enter), enters with action 4,
// waits for the host's verdict on the claim and switches a tab a tick (3 by the game's own tab call, 10 by the floppy
// tab's click handler). The viewer judges the LAST tab when a state carrying it arrives: PASS when its widget shows it,
// FAIL after 20 states without it or when the session ends; no clock decides. ABANDONED: it could not do its part.
//   tab / tab-host -- the client / the host holds; the other peer reaches tab 10. tab-norc -- the host reads the
//                     cameras tab (5) with createRC run 0 times. tab-join -- the host is on 4 before any client joins.
//   tab-handover   -- the client switches to 3 and leaves; the host reads 3, enters on it, switches to 6; the client reads 6.
//   tab-subgame    -- the client clicks the sub-game button; the host reads the tab canvas_subgame sits on.
//   tab-range / tab-forged -- a holding client sends an index past the last tab / a client holding nothing sends a
//                     report: the host counts a bad index / a not-holder refusal. tab-slot -- the host stamps a state
//                     with another sender's slot: the client counts a bad-slot refusal. tab-relay -- client 1 holds,
//                     client 2 reads, and client 1 is sent no state for its own reports.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::laptop_tab_drill {

// Game thread, once per pump tick; a latched string compare when off.
void Tick(coop::net::Session* session);

// The per-session half: a viewer still waiting for its verdict fails, and a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::laptop_tab_drill
