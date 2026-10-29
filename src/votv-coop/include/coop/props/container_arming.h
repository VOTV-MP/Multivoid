// coop/props/container_arming.h -- whether a world container's overlap box is ARMED, as the host's truth.
//
// prop_container_C's `overlap` trigger is NoCollision from spawn; the game arms it (QueryOnly) through the container's own
// dingus() and disarms it on broken(). A prop entering an unarmed box fires nothing. The truth is the host's own physical
// box, read where it is needed, never a logical bit that can drift (a roomba writes its box with no dingus; a box armed
// before hosting began has no edge to seed it):
//   - every publication reads it and carries it as the slice's flags byte (container_slice_wire::kFlagArmed);
//   - its CHANGE is caught where it happens: a native post hook on UPrimitiveComponent::SetCollisionEnabled, filtered by
//     the call's context being a tracked container's `overlap` component, marks the container for the next sweep;
//   - a client's mirror takes its box from the flag (ApplyFlags); its own local dingus is cancelled (container_act_put).
// A prop_roomba_C writes its own box from upd() on every peer, so a mirror's box is never written for it. Gameplay layer
// (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_arming {

// The session's start: the host installs its hook once the engine's function resolves (Tick retries until it binds).
void Install(coop::net::Session* session);
void Tick();
void OnDisconnect();

// The container's flags byte as this peer reads its box now: container_slice_wire::kFlagArmed when the box's collision is
// not NoCollision, else 0. 0 too for a null container and a class with no box.
uint8_t ReadFlags(void* container);

// The same reading as a question: is the box armed on this peer now?
bool IsArmed(void* container);

// Host: the container's `overlap` component joins the set the hook watches. Called at each publication, so a container is
// watched from before its first slice is sent; idempotent. A roomba is not watched: its own upd() writes its box, and its
// edge has no consumer. A container leaves the set at its end of play (and by Untrack), by address and eid together.
void Track(uint32_t eid, void* container);
void Untrack(uint32_t eid);

// Client: the mirror's box takes the host's flag, QueryOnly when armed and NoCollision when not. A roomba's, whose
// own upd() writes it, is left alone. True when the box matches the flag afterwards (or is a roomba's); false when it did
// not read or the write failed, so the caller does not record the flag as applied.
bool ApplyFlags(uint32_t eid, void* container, uint8_t flags);

}  // namespace coop::props::container_arming
