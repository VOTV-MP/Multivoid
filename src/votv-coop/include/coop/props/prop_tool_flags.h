// coop/props/prop_tool_flags.h -- the toolgun's prop-properties tool as a request the host performs. Overview:
// docs/props.md.
//
// tool_propProps_C::init(toolgun) is the whole press: it calls setPropProps(isStatic, isFrozen, isActive, isSleeping)
// on the prop the toolgun's outHit names, then destroys itself, so a client's press changed its own copy alone. A
// client now cancels the body and sends PropPropsIntent: the element it hit, the camera point it looked from, the point
// its shot struck and the tool's four values. A hit that names no published prop runs natively.
//
// The host queues a client's presses (the toolgun lanes' shared limits), judges each in the remover's order plus a
// toolgun in the sender's hand, applies the tool's values through the game's own setter, and publishes the prop's
// flags read back from the actor (PropPropsState) to every peer. The host's own press is published from the tool's post
// row. A refused press is answered to its sender alone with the reason. The right is the node
// multivoid.gameplay.toolgun.propprops, default granted.
//
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropPropsIntentPayload;
struct PropPropsStatePayload;
}  // namespace coop::net

namespace coop::prop_tool_flags {

// Cache the session and take the init watch on tool_propProps_C (once per process). Both roles.
void Install(coop::net::Session* session);

// Settle the watch's registration with one line; on a running host, serve each sender's queue head and flush the
// refusal tally. Game thread.
void Tick();

// HOST: a client's press, queued. An unknown sender or a full queue is refused into the tally. Game thread.
void OnIntent(const coop::net::PropPropsIntentPayload& payload, uint8_t senderSlot);

// HOST: a refusal the router made (a short payload), into the lane's tally.
void NoteRefused(uint8_t senderSlot, const char* why);

// CLIENT: the host's flags for a prop, set on this peer's copy through the game's own setter (an unknown element is
// dropped), or its refusal of this player's press, shown when visible. Game thread.
void OnState(const coop::net::PropPropsStatePayload& payload);

// The tally emptied, the shown notices forgotten and the pending press dropped. Net disconnect.
void OnDisconnect();

}  // namespace coop::prop_tool_flags
