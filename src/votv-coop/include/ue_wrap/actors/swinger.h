// ue_wrap/swinger.h -- standalone engine access for VOTV openable container lids
// (Aprop_swinger_C). Principle-7 engine-wrapper layer (no network/coop state).
//
// Aprop_swinger_C is the generic hinged-lid prop: fridge doors, safe doors,
// microwave doors, cabinet / cupboard / drawer lids all inherit it. Its open
// state is `opened`; the canonical entry points are Open(bool Damage) / Close().
// It is an Aprop_C, so its cross-peer identity is the inherited Aprop_C::Key
// (read via ue_wrap::prop::GetKeyString) -- NOTE: a child-actor lid (e.g. a
// fridge door spawned by its parent) may carry a per-peer NewGuid Key that is
// NOT cross-peer stable; coop::interactable_sync's install-time keysHash
// diagnostic surfaces that, and an unresolvable key simply expires harmlessly.
//
// RE: research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md +
// CXXHeaderDump/prop_swinger.hpp.

#pragma once

namespace ue_wrap::swinger {

// Resolve prop_swinger_C + the `opened` offset + the Open / Close UFunctions.
// Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is prop_swinger_C or a subclass (fridge/safe/microwave/
// cabinet door, etc.). False if not yet resolved.
bool IsSwinger(void* obj);

// Read the lid's `opened` bool into `on`. False if the read could not be made.
bool TryReadOpen(void* swinger, bool& on);

// Open(damage) / Close(). MUST run on the game thread. `damage` is the BP's
// Open(bool Damage) param -- false for a normal (player/receiver) open. Return
// false on null / unresolved UFunction.
bool CallOpen(void* swinger, bool damage);
bool CallClose(void* swinger);

// The Open / Close UFunction pointers (for POST-observer registration). nullptr
// until EnsureResolved.
void* OpenFn();
void* CloseFn();

}  // namespace ue_wrap::swinger
