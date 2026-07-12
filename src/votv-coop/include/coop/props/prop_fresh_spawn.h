// coop/props/prop_fresh_spawn.h -- the fresh mirror MATERIALIZER: build a brand-new local actor
// from a wire PropSpawn payload. Extracted from remote_prop_spawn.cpp 2026-07-12 (the audit-owed
// modularity split: that file carried the RECEIVER -- dedup/converge/fuzzy binding -- AND this
// deferred-spawn pipeline; two concepts, two files).
//
// ONE concept: the deferred-spawn pipeline for a wire-expressed prop with no local match --
//   BeginDeferredActorSpawnFromClass -> claim -> setKey(wire key) BEFORE Finish -> SP-parity
//   identity row write -> echo-suppress mark -> FinishSpawningActor -> ambient-mirror lifespan
//   backstop -> trash variant stamp -> SP-parity physics -> mirror bind + key index + defer-hide.
// Call sites: remote_prop_spawn::OnSpawn's no-match tail; remote_prop::OnConvert reaches it
// through OnSpawn (skipBind=true, binds E itself).
//
// Game thread only.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::prop_fresh_spawn {

// Materialize the payload as a fresh local actor. classW/keyW/propNameW are the caller's already-
// decoded wire strings (OnSpawn decodes them once for all its paths). skipBind: the OnConvert
// morph binds eid E itself (RegisterPropMirror rebindInPlace) -- skip the default bind/index/
// defer-hide tail. Returns the spawned actor, or nullptr on any failure (unresolved spawn fns /
// unknown class / no world / engine call failure). Game thread only.
void* Materialize(const coop::net::PropSpawnPayload& payload, int senderSlot,
                  const std::wstring& classW, const std::wstring& keyW,
                  const std::wstring& propNameW, bool skipBind);

// The Aprop_C-base setKey UFunction (resolved + cached; nullptr while the Aprop_C UClass is not
// yet loaded -- the engine loads BP classes on demand, retried per call). Consumed by the
// Gap-I-1 fuzzy-rekey path in remote_prop_spawn (the leaf-safe base resolve,
// [[lesson-findfunction-exact-owner-no-superstruct-climb]]). Game thread only.
void* PropSetKeyFn();

}  // namespace coop::prop_fresh_spawn
