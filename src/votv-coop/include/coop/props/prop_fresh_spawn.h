// coop/props/prop_fresh_spawn.h -- the fresh mirror MATERIALIZER: build a brand-new local
// actor from a wire PropSpawn payload.
//
// ONE concept: the deferred-spawn pipeline for a wire-expressed prop with no local match --
//   BeginDeferredActorSpawnFromClass -> claim -> setKey(wire key) BEFORE Finish -> SP-parity
//   identity row write -> echo-suppress mark -> FinishSpawningActor -> ambient-mirror lifespan
//   backstop -> trash variant stamp -> SP-parity physics -> mirror bind + key index +
//   defer-hide.
// The RECEIVER half -- dedup, converge and fuzzy binding -- stays in remote_prop_spawn.cpp.
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

// The Aprop_C-base setKey UFunction (resolved and cached; nullptr while the Aprop_C UClass is
// not yet loaded -- the engine loads BP classes on demand, so it is retried per call).
// Consumed by the fuzzy-rekey path in remote_prop_spawn, which needs the base resolve because
// FindFunction matches the exact owner and does not climb the superclass chain. Game thread
// only.
void* PropSetKeyFn();

}  // namespace coop::prop_fresh_spawn
