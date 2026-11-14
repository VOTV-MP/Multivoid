// coop/props/roomba_sync.h -- the robot cleaner (prop_roomba_C) is the host's: where it drives, whether it runs, and
// what it picks up.
//
// The cleaner drives itself by physics: its tick turns its wheels, a random roll and its bumper's hits pick its turns,
// and its overlap takes items out of the trash piles it passes into its own container. Each peer ran that on its own
// copy, so the copies drove apart. Here a client refuses the cleaner's tick and its pickup at the script gate. The host
// watches the cleaner's `use`, which the switch's press and the save's load both end in: each change of `active` is
// sent (RoombaState) and the host claims the running cleaner's drive (coop/props/prop_drive_host) or releases it, so
// every client's copy follows the host's pose. A cleaner is announced with its `active` when its element id is minted,
// and to a joiner at its ready edge. A client animates the brushes of the copies it has bound, takes a state through
// the cleaner's own `use`, and holds the newest state of a cleaner whose copy is not bound yet until it is. A client's
// press of the switch (action 5) is refused and sent to the host (RoombaIntent), which judges its reach (a press whose sender has no puppet yet waits, in order, up to 64 of them, until the slot leaves) and presses
// its own cleaner. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct RoombaPayload;
}  // namespace coop::net

namespace coop::roomba_sync {

// Keep the session; register the watches on the cleaner's tick, pickup, actions and `use`, and subscribe to the host's
// prop births. Idempotent.
void Install(coop::net::Session* session);

// HOST: announce the cleaners born, judge the queued presses. CLIENT: animate the bound copies' brushes. Settles the
// watches. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every cleaner's `active`, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's press, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::RoombaPayload& p);

// CLIENT: the host's state, its length checked by the dispatcher.
void OnState(const coop::net::RoombaPayload& p);

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`; `cls` is its class name. A cleaner's copy
// joins the brush list and takes the state held for it.
void OnBound(uint32_t eid, void* actor, const wchar_t* cls);

// CLIENT: the world changed under the session: the bound list and the held states of the old one go.
void OnClientWorldReady();

// Session end: the records are cleared.
void OnDisconnect();

// [dev] What the lane has done since the process began (monotonic, so a drill's delta across a session end never
// underflows), for the cleaner drill. HOST: states sent, presses it switched for a client. CLIENT: states
// received and applied (a flip through `use`), presses asked of the host, states held for an unbound copy, and the id of
// the cleaner the last state named.
struct Counters {
    uint32_t statesSent = 0, intentsSwitched = 0, statesReceived = 0, statesApplied = 0, intentsSent = 0, held = 0,
             lastStateEid = 0;
};
Counters Read();

}  // namespace coop::roomba_sync
