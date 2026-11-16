// coop/props/hacksaw_cut.h -- a client's hacksaw cuts a prop only once the host has granted the cut.
//
// The saw (prop_hacksaw_C) runs only where it is held, and the tick that brings its `complete` to 1 spawns the scrap,
// calls the prop's `broken`, destroys it and ends the cut, in one call: on a client a container's spill would be that
// client's alone while the destroy took the host's copy. So on a client the script gate's PRE on the saw's ReceiveTick
// reads the sum the Blueprint will, and on the tick that would complete the cut of a keyed prop it cancels the body and
// asks the host (coop/props/container_break, op 1, sawn); every completing tick is cancelled while the request waits.
// GRANTED (op 3): the host records it, and the next completing tick runs the game's own completion: the scrap
// (its births cross as the player's), `broken` (cancelled here: the host runs it when the destroy arrives), the destroy
// (relayed), completeSaw.
// REFUSED (op 2): the game's own hint "Cannot saw this prop!" and the saw's completeSaw, so nothing asks again. An
// unkeyed prop is not shared state and is cut locally. The latch is keyed on the prop's Key and the saw, and ends at
// completeSaw (a POST watch), the world's change and the session's end. A granted cut that ends with its prop standing hands
// the grant back to the host (container_break, op 4), which ends its record. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::props::hacksaw_cut {

// On a client session, register the watches on the saw's ReceiveTick, completeSaw and playerHandUse_LMB and on `broken`
// (the script gate's name watches). Idempotent and retried a bounded number of times; called every load tick. A give-up
// retires every watch that did register, so the saw runs wholly native.
void Install();

// CLIENT: the host granted the cut of the prop keyed `propKey` (container_break, op 3), or refused it (op 2).
void OnGranted(const std::wstring& propKey);
void OnRefused(const std::wstring& propKey);

// What the PRE on the saw's ReceiveTick has seen on this client since the session began: calls, and those it cancelled.
uint32_t TickCalls();
uint32_t TickCancels();

// Hands back a grant whose cut ended without its destroy (the saw's target changed, the saw is gone, the world changed), puts
// back a copy the saw's start set simulating when its POST was skipped, and forgets the cut when the world changes. Game thread.
void Tick();

// The session ended: the counts are said and the cut is forgotten.
void OnDisconnect();

}  // namespace coop::props::hacksaw_cut
