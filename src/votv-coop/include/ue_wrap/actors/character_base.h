// ue_wrap/actors/character_base.h -- what a character's own movement says it stands on. The engine's movement tick writes
// ACharacter::BasedMovement.MovementBase (the primitive component under the capsule's feet, null in the air or on ground
// the movement does not track) while it walks; a puppet whose movement tick is parked is never written, so its base stays
// what it was. Read-only; the engine's layout is resolved by reflection once and the offsets kept.
// Engine-wrapper layer: no coop state. Game thread.

#pragma once

namespace ue_wrap::character_base {

struct Base {
    void* component = nullptr;   // the primitive component the character stands on, null when it stands on none
    void* owner = nullptr;       // that component's owning actor (its outer), null when it has none
};

// ACharacter::BasedMovement.MovementBase of `character`. True when it was read (`out` empty when the character stands on
// nothing); false for a null or dead object, one that is not a Character, or an engine layout that did not resolve (said
// once).
bool ReadMovementBase(void* character, Base& out);

// The component alone (`Base::component`), for a caller that compares it with the one it saw last before it asks for the owner.
// Null for none and for an unreadable character.
void* MovementBaseComponent(void* character);

// ACharacter::CapsuleComponent, or null when `character` is not a live Character.
void* CapsuleComponent(void* character);

}  // namespace ue_wrap::character_base
