// ue_wrap/world/keyed_objects.h -- the GAME's own key-to-actor registry.
//
// mainGamemode_C's `getObjectFromKey` is how VOTV turns a save key back into a live actor, and it
// is what every save-restore path uses -- including hook_C's processKeys, which resolves both of a
// hook's attach keys through it.
//
// It exists here as a wrapper, separate from our own `coop/props/prop_key_index`, because the two
// answer different questions. Ours indexes the keyed props WE track; this one is the game's and
// covers everything that implements `int_objects_C`. A lane that has to predict what the game will
// resolve -- an arbiter validating a key before handing it back to a save-restore verb, say -- has
// to ask the game, or it validates one actor and the game attaches to another.
//
// Game thread (ProcessEvent).

#pragma once

namespace ue_wrap::keyed_objects {

// The live actor the game would resolve `key` to, or null. A `"None"` or unknown key is null, which
// is also what the game does with it -- an unresolvable attach key leaves the hook tied to nothing
// rather than to a substitute.
void* Resolve(const wchar_t* key);

// Drop the cached CDO and UFunction (level change / disconnect).
void ResetCache();

}  // namespace ue_wrap::keyed_objects
