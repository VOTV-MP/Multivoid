// coop/dev/container_opener_probe.h -- [dev] the actors a far-standing container is opened through, found
// on the world's real objects, and the host's reach verdict through them. On the host, once a client's world
// comes up, the object index holds the world and every ready client has a body: every drone, dronesack and
// ATV has the container its field names read, ue_wrap::container_openers is asked both ways -- which actors
// open that container, and which container that actor opens -- and must answer with the pair the pass
// started from; for each client the host's own reach verdict (the one container_write_policy judges a slice
// by) to the container and to the opener is logged, and a verdict line closes the pass. The reach is judged
// again on the tick a client's body comes to stand at an opener, where a slice through it would be judged.
// Armed by container_opener_probe=1; read-only.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_opener_probe {

// The gameplay tick's entry: one latched flag read when off. Host, game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::container_opener_probe
