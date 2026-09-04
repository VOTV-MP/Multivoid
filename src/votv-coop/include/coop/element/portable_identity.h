// coop/element/portable_identity.h -- WHAT NAMES AN ENGINE ACTOR ACROSS TWO PROCESSES.
//
// The game's own instance Key is NOT a portable identity. `lib_C::assignKey` mints a
// random 16-byte -> base64url FName for any `triggerBase` whose Key is None at load
// (research/bp_reflection/cpp/lib.cpp:5558-5623, :8698), and `Aprop_C`'s UCS mints a
// NewGuid on the same condition -- both PER PROCESS. Measured 2026-09-05 on a two-peer
// rig: 110 interactables per peer carry a Key the other peer has never heard of (door
// 31 of 50, light 27 of 42, lightgroup 27 of 42, container 25 of 56), which is field
// defect B2 -- a client's radiotelescope door that no host state can ever open.
// Full RE: research/findings/join-identity/votv-portable-interactable-identity-RE-2026-09-05.md
//
// This module answers the identity question at OUR layer (principle 3: our parallel
// hierarchy owns network identity, the engine object owns rendering/physics/state). It
// NEVER writes the game's Key -- that route was measured shut twice over: the mint runs
// inside a UserConstructionScript at map load so it cannot be pre-empted, and rewriting
// it afterwards collides with prop_element_tracker's host-only re-key invariant
// (`CLIENT never re-keys`) and with the take-7 child-actor exclusion (docs/LESSONS.md).
//
// THE RULE (recursive, total, structural):
//
//   portable(a) = a is a CHILD ACTOR -> portable(parent(a)) + "/" + <component name>
//                 RF_WasLoaded(a)    -> "n:" + <UObject name>     // baked into the cooked level
//                 Key(a) != None     -> "k:" + <Key>              // top-level: save-persisted
//                 otherwise          -> ""                        // NO identity; say so, never guess
//
// Uniqueness is STRUCTURAL, not measured: UE requires component names unique within an
// actor, so two children of one parent differ by component and two children of different
// parents differ by the parent's identity. (Measured anyway: 73 parents with children,
// 34 with more than one, ZERO duplicate component names, on both peers.)
//
// Measured outcome of the rule on the b2ident3 run: 109 of the 110 broken instances
// resolve, host and client each map 272 instances to 272 DISTINCT identities with zero
// collisions, and ZERO of the 163 already-working instances change meaning. The one that
// does not resolve is reported as "" -- a crematorium door whose parent is itself
// top-level, runtime-created and keyless; closing it needs the element/eid layer.
//
// Game thread only: the reads are array-slot and property reads, but `ParentActorOf`
// resolves a weak pointer against GUObjectArray and the caller is expected to hold the
// engine still (the scan hub's pass, an input observer).

#pragma once

#include <cstdint>
#include <string>

namespace coop::element {

// The readable portable identity of `actor`, or "" when it has none.
// Diagnosis-facing: this is what goes in the log beside the wire token.
std::wstring PortableIdentity(void* actor);

// The wire form: "mv_" + 16 lowercase hex of FNV-1a-64 over the readable identity.
// 19 characters, so it fits the 31-char WireKey with room to spare, and it is derived
// by pure computation -- two peers with the same world produce the same token without
// exchanging anything. "" when the actor has no portable identity.
//
// The prefix is deliberate and diagnosable: a `mv_` key in a log is OURS, an `rk_`/`cs_`
// key is prop_synth_key's, and anything else is the game's. NOTE the contrast with
// prop_synth_key's `rk_`, which is RANDOM on purpose because it PERSISTS into the save;
// ours is DETERMINISTIC on purpose because it must be identical on two machines and is
// never written into the game at all.
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
