// ue_wrap/actors/prop_record.h -- a prop's save state as the game itself saves it: the bytes of its own betterGetData
// record, handed back through its own betterLoadData.
//
// The game's persistence is the pair betterGetData / betterLoadData. betterGetData fills a struct_betterSave with the
// actor's identity (class, transform, name, tag, key, attachKey, index) and, in actorData, the stream the native
// ThingsAndStuffLib::ActorToBytes writes: every SaveGame property of the actor, the base prop's and each class's own.
// betterLoadData reads that stream back with BytesIntoActor, then re-seats the identity fields and re-runs init(). A
// class that keeps state elsewhere (an attached socket, a component) declares its own betterGetData to put it where
// ActorToBytes reaches. So the state of a prop is the actorData and nothing else here is worth carrying: the identity
// half of a record names the actor it came from, and on the receiving peer that actor already exists.
// Capture returns the actorData. Apply builds the record betterLoadData takes from the RECEIVER'S OWN betterGetData (so
// the class, transform, name, tag, key, attachKey and index are the receiver's, never the sender's: a record is not a
// primitive for renaming a prop or moving it) and swaps the sender's actorData in. The stream is applied as the game
// applies a save file's; its remaining-lifespan field alone is put back afterwards, since SetLifeSpan destroys the actor
// and the lifespan, like the identity, is the receiving peer's own clock. Engine-wrapper layer (principle 7). Game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace ue_wrap::prop_record {

// True when instances of `cls` carry save state of their own: `cls` is on the Aprop_C chain and a class below
// Aprop_C declares a SaveGame property or a betterGetData of its own. Read from the live class chain and cached
// per class, so a save-backed prop the game adds is carried without a change here, and a prop with only the base
// record (whose every field rides its spawn row) is not.
bool CarriesState(void* cls);

// `actor`'s actorData as its own betterGetData leaves it. False when the class carries no state of its own, the
// struct_betterSave layout does not check out, or the call fails.
bool Capture(void* actor, std::vector<uint8_t>& bytes);

// Hand `bytes` to `actor` through its betterLoadData, as above. The dispatch's success, not the Blueprint's: the
// bool betterLoadData returns is not read. False when the class carries no state of its own, `bytes` is empty, the
// layout does not check out, or a call fails.
bool Apply(void* actor, const std::vector<uint8_t>& bytes);

// True while Apply runs: what a prop's own bodies do then is a record arriving, not a change to report.
bool IsApplying();

// Drop the cached class answers and the layout (level change / disconnect).
void ResetCache();

}  // namespace ue_wrap::prop_record
