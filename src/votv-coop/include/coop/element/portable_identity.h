// coop/element/portable_identity.h -- WHAT NAMES AN ENGINE ACTOR ACROSS TWO PROCESSES.
//
// The game's own instance Key is NOT a portable identity. `lib_C::assignKey` mints a random 16-byte
// base64url FName for any `triggerBase` whose Key is None at load, and `Aprop_C`'s
// UserConstructionScript mints a NewGuid on the same condition -- both PER PROCESS. On a two-peer
// rig, 110 interactables per peer carry a Key the other peer has never heard of (31 doors of 50, 27
// lights of 42, 27 light groups of 42, 25 containers of 56), which is what leaves a client's
// radiotelescope door unopenable by any host state.
//
// This module answers the identity question at OUR layer -- principle 3, where our parallel
// hierarchy owns network identity and the engine object owns rendering, physics and state. It NEVER
// writes the game's Key: that route is shut twice over, since the mint runs inside a
// UserConstructionScript at map load and so cannot be pre-empted, and rewriting it afterwards
// collides with prop_element_tracker's host-only re-key invariant and with the child-actor
// exclusion.

#pragma once

#include <cstdint>
#include <string>

namespace coop::element {

// The readable portable identity of `actor`, or "" when it has none. Diagnosis-facing: this is what
// goes in the log beside the wire token. THE RULE is recursive, total and structural:
//
//   portable(a) = a is a CHILD ACTOR -> portable(parent(a)) + "/" + <component name>
//                 Key(a) != None     -> "k:" + <Key>              // tried FIRST; save-persisted
//                 RF_WasLoaded(a)    -> "n:" + <UObject name>     // baked into the cooked level
//                 otherwise          -> ""                        // NO identity; never a guess
//
// The key wins the name because a level-placed anchor is replaced by its save-loaded twin mid-load.
//
// Uniqueness is STRUCTURAL rather than measured: UE requires component names to be unique within an
// actor, so two children of one parent differ by component and two children of different parents
// differ by the parent's identity. The residual case that returns "" is an actor which is itself
// top-level, runtime-created and keyless -- a crematorium door is one -- and naming that needs the
// element and eid layer instead.
std::wstring PortableIdentity(void* actor);

// The wire form: "mv_" + 16 lowercase hex of FNV-1a-64 over the readable identity. 19 characters,
// so it fits the 31-char WireKey with room to spare, and it is derived by pure computation: two
// peers with the same world produce the same token without exchanging anything. "" when the actor
// has no portable identity. Game thread, like PortableIdentity, whose `ParentActorOf` resolves a
// weak pointer against GUObjectArray and so needs the engine held still.
//
// The prefix is deliberate and diagnosable: a `mv_` key in a log is OURS, an `rk_` or `cs_` key is
// prop_synth_key's, and anything else is the game's. Note the contrast with prop_synth_key's `rk_`,
// which is RANDOM on purpose because it PERSISTS into the save; ours is DETERMINISTIC on purpose
// because it must be identical on two machines and is never written into the game at all.
std::wstring PortableWireKey(void* actor);

// The hash, exposed for a caller that already holds the readable form.
uint64_t IdentityHash(const std::wstring& readable);

// UN-GATED per-session selftest over the pure half -- the hash and the wire-token format.
// It exists because the whole design rests on TWO MACHINES computing the same 19 characters
// from the same string, and nothing else in this project ever checks that: a hash that
// silently depended on wchar_t's width or the host's endianness would make every peer agree
// with itself and with nobody else, which reads as "the identity lane does not work" and
// points at the identity rule rather than at the hash. Logs
// `portable_identity selftest: ALL PASS (N checks)` or an ERROR line; mp.py asserts it on
// both peers. Returns true when every check passed. Callable off the game thread (pure).
bool RunSelfTest();

}  // namespace coop::element
