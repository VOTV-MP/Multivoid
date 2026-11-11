// coop/dev/container_opener_probe.h -- [dev] the actors a far-standing container is opened through, found
// on the world's real objects, and the host's reach verdict through them. On the host, once a client's world
// comes up, the object index holds the world and every ready client has a body: every ATV has the container
// its field names read, ue_wrap::container_openers is asked both ways -- which actors
// open that container, and which container that actor opens -- and must answer with the pair the pass
// started from; for each client the host's own reach verdict (the one the container act judges a request
// by) to the container and to the opener is logged, and a verdict line closes the pass. The reach is judged
// again on the tick a client's body comes to stand at an opener, where a request through it would be judged.
// In 0.10 an ATV opens its own inventory component (its trunk, a shared target of the container act lane) and not the
// `spawnedContainer` prop of 0.9: each ATV with a trunk is a pair of its own, named both ways by
// coop/interactables/atv_trunk_sync, and each client's reach is judged through coop/props/container_reach.
// Armed by container_opener_probe=1; read-only.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_opener_probe {

// The gameplay tick's entry: one latched flag read when off. Host, game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::container_opener_probe
