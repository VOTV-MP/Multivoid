// coop/props/prop_tool_flags.h -- a toolgun tool's freeze, unfreeze or make-static reaches every copy of the
// prop. Overview: docs/props.md.
//
// A toolgun tool is a tool_*_C actor whose init(toolgun) acts on the prop the toolgun's outHit names, on its
// user's machine alone, so the flags it sets (static, frozen, sleep) stayed on that copy. Every tool_*_C's
// init is watched at the script gate, so a tool this lane has never named is covered: before the body the
// target's flags are read, after it they are read again, and a change goes out as PropToolFlags. A receiver
// sets the same flags through prop_C's own setter, as the release and the snapshot converge them, unless its
// own player holds the prop. The remover is a tool too and changes no flags; its own lane carries it.
//
// Game thread.

#pragma once

namespace coop::net {
class Session;
struct PropToolFlagsPayload;
}  // namespace coop::net

namespace coop::prop_tool_flags {

// Cache the session and register the watch on every class's init, once for the process. Idempotent.
void Install(coop::net::Session* session);

// Per gameplay tick: resolve the watch's names until it is live, and say once whether it is. Game thread.
void Tick();

// RECEIVER: a peer's tool changed a prop's flags. Validated at the router.
void OnToolFlags(int senderSlot, const coop::net::PropToolFlagsPayload& p);

// Session end: the counters and the pending read. Game thread.
void OnDisconnect();

}  // namespace coop::prop_tool_flags
