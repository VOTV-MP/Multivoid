// coop/interactables/woodchipper_feed.h -- a prop a client feeds a woodchipper is eaten by the host's.
//
// A client's chipper copy runs no intake (coop/interactables/woodchipper_gate), and a prop the client holds or syncs is
// on the host only as the copy its stream drives, out of the host's simulation, which the host's chipper refuses as it
// enters the eat box (canChip wants a simulating body). Once the client lets go, the host moves its copy to where the
// client's came to rest (coop/props/prop_settle), and the client's never stops in the box, since nothing eats it there,
// so the host's chipper eats it only if its copy happens to fall through the box while it simulates.

// So the client's copy is the witness: when its chipper copy's eat box takes a prop this client holds or syncs, the
// gate's refused intake asks the host (WoodchipperIntent, op Feed, with the prop's key) and a held prop leaves the
// hand, as the chipper takes it from a held grab in single player. The host judges it -- the sender's world, life and
// reach of the chipper, the chipper running, the prop a prop that nobody else drives, its own copy within 3 m of the
// box (1.5 s given to catch up) -- waits for the sender's hold and the stream of the falling copy to end on its copy (5 s
// at most), then puts its copy in its own eat box, simulating, and runs the chipper's own intake on it, so the game eats
// the prop, chips it and drops its trash on the host, and the prop's destroy and the trash reach every peer by the prop
// lanes. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WoodchipperPayload;
struct WoodchipperFeedTail;
}  // namespace coop::net

namespace coop::woodchipper_feed {

// CLIENT, from the gate's refused intake on `chipper`: `other` entered its eat box. Asks the host when `other` is a
// keyed prop this client holds or syncs and the copy shows the chipper running; once a second a prop at most.
void OnClientIntake(void* chipper, void* other);

// HOST: a client's feed, queued for the next tick.
void OnFeed(int senderSlot, const coop::net::WoodchipperPayload& p, const coop::net::WoodchipperFeedTail& tail);

// HOST: the queued feeds judged and done. Game thread, from the chipper lane's tick.
void Tick(coop::net::Session& s);

// Session end: the counts are said and the queues forgotten.
void OnDisconnect();

// For drills: what the lane did this session.
struct Counts {
    uint32_t asked = 0;    // CLIENT: feeds sent
    uint32_t fed = 0;      // HOST: feeds done, the chipper's intake run on its copy
    uint32_t refused = 0;  // HOST: feeds refused
};
Counts GetCounts();

}  // namespace coop::woodchipper_feed
