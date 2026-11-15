// coop/props/container_writer_watch.h -- the edge half of the container contents lane: the one
// module that turns a write to a container's inventory into a dirty mark. The script gate watches
// the inventory component's writers by name -- addItemAsObject, addItemAdvanced, removeInventoryItem,
// trimInventory, and transferFromInvToInf, which writes the component it is handed -- and a hit on a
// world container's inventory resolves the owner's element id. On the host it goes to
// container_contents_sync::MarkHostWrite, which the lane's sweep drains; on a client the mirror is
// display only, so the hit forgets the applied hash and names the writer to the dev probe, and marks
// nothing. The verbs are local virtual calls (EX_LocalVirtualFunction), so only the script gate sees
// them; docs/coop-dispatch-visibility.md has the row.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::container_writer_watch {

// Hands the watch the session its callbacks read the role and the connection from.
void Install(coop::net::Session* session);

// Registers the watches until the gate has taken them; one failure line per process. The name
// resolve itself (script_gate::ResolvePendingNames) belongs to the caller's tick. Game thread.
void Tick();

// Session end: the first-entry latch is per session.
void OnDisconnect();

// Read and clear the extraction latch: set while the inventory screen's dropItems runs, the body that spawns an item
// out of an inventory into the world. prop_drop_intent::OnClientFinishSpawn consumes it to mark that spawn as an
// extraction. One-shot by exchange; also reached on a task-graph worker.
bool ExtractInFlight();

}  // namespace coop::props::container_writer_watch
