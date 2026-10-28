// coop/dev/container_writer_probe.h -- [dev] names a writer of a world container that the container lane never saw.
// On the host, every 2 s, the probe reads each world container's content hash and keeps its own last reading beside
// the lane's touch epoch for that container (container_contents_sync::TouchEpochOf: a local write, an applied
// remote slice, a connect seed's send). A hash that differs from the probe's last reading while the epoch did not
// move changed through something the lane did not see, and the probe says so once per container. A container's
// first reading only primes. The sweep starts once the host's world has settled (host_world_settle). Read-only.
// The client half names a verb that changed a shared container's mirror outside an asked act: the container
// writer watch hands it each such entry, and it says once per (container, outermost script function).
// Armed by container_writer_probe=1.
#pragma once

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap::script_gate { struct Call; }

namespace coop::dev::container_writer_probe {

// The gameplay tick's entry: one latched flag read when off. Host, game thread.
void Tick(coop::net::Session* session);

// A client's addObject (take=false) or takeObj (take=true) on the shared container `eid`, outside a granted act's
// replay. A takeObj is a warning: its item still reaches the host while the host keeps the record. Counted per
// container whatever the row says; said only with the row on. Game thread.
void OnClientVerb(uint32_t eid, void* owner, const ue_wrap::script_gate::Call& c, bool take);

// How many client verbs OnClientVerb has counted on this container since the session began: the witness a drill
// reads to know the verb watch fired. Counts at most kMaxClientKeys containers. Game thread.
uint32_t ClientVerbsOf(uint32_t eid);

// The session's end: the readings belong to one world.
void OnDisconnect();

}  // namespace coop::dev::container_writer_probe
