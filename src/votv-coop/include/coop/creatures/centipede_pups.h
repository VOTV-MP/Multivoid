// coop/creatures/centipede_pups.h -- a client's copy of Wolfgang mints no pups of its own.
//
// centipedeHead_C's begin play mints his body (the segments, which a copy needs) and, for a head born while a parentWolfgang_C
// stands in the world, eight pups (wolfgangJr_C, a subclass of the head) with `babyfollow` set to him. A parentWolfgang_C is a
// save actor, so a joiner's world holds the host's, and a copy's begin play, which runs whole (coop/creatures/npc_mirror_brain),
// would mint its own eight beside the eight the host's head minted and the lane mirrors. A pre hook on the deferred spawn
// refuses a spawn of the head's family whose source is the copy. The hook is armed from the copy's birth (npc_mirror_brain's
// PrepareBirth, before the finish runs its begin play) to the end of that begin play (SetUpPresentation, right after the
// finish), and refuses nothing while it is disarmed, so every other spawn of the game passes untouched.

#pragma once

namespace coop::centipede_pups {

// CLIENT, game thread: `head` is a mirror of Wolfgang or of a pup, about to be finished; its own mints are refused until it is
// begun. No other family is armed, and the hook installs on first use.
void OnMirrorBorn(void* head);

// CLIENT, game thread: the finish returned, so the begin play that could mint has run. Disarms, and says how many mints it
// refused when there were any.
void OnMirrorBegun(void* head);

// The session ended: disarmed.
void OnDisconnect();

}  // namespace coop::centipede_pups
