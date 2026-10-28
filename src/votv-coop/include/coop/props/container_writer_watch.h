// coop/props/container_writer_watch.h -- the edge half of the container contents lane: the one
// module that turns a write to a container's inventory into a dirty mark. The script gate watches
// addObject and takeObj by name; a hit on a world container's inventory resolves the owner's
// element id and hands it to container_contents_sync::MarkHostWrite, which the lane's sweep
// drains. The verbs are local virtual calls (EX_LocalVirtualFunction), so only the script gate
// sees them; docs/coop-dispatch-visibility.md has the row. One writer calls neither verb: the
// drone's sale (droneSellLocation_C::sell) clears its container's save-slot row directly, so the
// host watches that body too and marks the container it emptied.

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

// Read and clear the takeObj-in-flight latch, armed at the dispatch edge.
// prop_drop_intent::OnClientFinishSpawn consumes it to mark the extracted item's spawn as a
// container extraction. One-shot by exchange; also reached on a task-graph worker.
bool TakeObjInFlight();

// Has the addObject/takeObj watch fired on this peer at least once? A dev instrument's verdict
// needs it: an absent effect means one thing if the edge is live and another if it never was.
bool VerbWatchEntered();

}  // namespace coop::props::container_writer_watch
