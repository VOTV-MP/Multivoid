// ue_wrap/actors/prop_events.h -- the base prop class's own Blueprint events, run on a prop the way
// the game runs them on the machine where they happen: a grab's prelude and a throw. Each call runs
// prop_C's own function, never a subclass's override of it, so a receiver replays what the base does
// and nothing a subclass adds; the two readers at the end ask the prop's own class instead. Engine-wrapper layer
// (principle 7): no network or coop state.
// Implementation: src/ue_wrap/actors/prop_events.cpp.

#pragma once

namespace ue_wrap::prop {

// prop_C's playerGrabbed_pre, the first thing a grab runs on the prop: its lifespan cleared, its
// `touched` broadcast, and awakeUnfreeze when it is frozen or asleep. The base body never reads its
// player, so none is passed. False for a null prop or when the class or the event did not resolve.
// Game thread.
bool CallBaseGrabPrelude(void* prop);

// prop_C's own setPropProps(static, frozen, active, sleeping): the flags written and init() run. The
// base body never reads `active`, where a subclass's override does, as its own state -- a spotlight's
// on switch, an explosive's arming, the plasma TV's stick switch -- and forces sleep off on the
// wall-attach component's owners, so a flag that only the physics state should change is written
// through the base. False for a null prop or when the class or the function did not resolve. Game
// thread.
bool CallBaseSetPropProps(void* prop, bool isStatic, bool frozen, bool sleeping);

// prop_C's thrown(Player) event, run as a throw runs it, so the Blueprint plays the swing's trail and
// sound. False for a null prop or player, or when the class or the event did not resolve. Game
// thread.
bool CallPropThrown(void* prop, void* player);

// The two pure answers prop_container's putObjectIn_overlap reads of a prop before it files it (it returns silently on
// either): the prop's ignoreSave, and its canPickup (not static, not frozen, and its propData allows holding it). Each
// is the BP function of that name on the prop's own class, so an override answers. False, `out` untouched, for a null or
// dead prop and for a class without the function (an actor without the interface takes the body's cast branch), so a
// caller reads an unresolved answer as no answer and never as a pass. Game thread.
bool ReadIgnoreSave(void* prop, bool& out);
bool ReadCanPickup(void* prop, bool& out);

}  // namespace ue_wrap::prop
