#pragma once

#include <string>

// coop/props/remote_prop_spawn.h -- the wire-driven PropSpawn receiver: the OnSpawn pipeline that
// materializes a wire-received Prop on the receiving side. It validates the payload, then tries an
// EXACT-KEY dedup, then a FUZZY-POSITION one (a same-class actor within 30 cm, rekeyed through
// Aprop_C.setKey), and only then a fresh spawn; it ends by registering the actor as a Prop mirror
// at the sender's eid, unless `skipBind` below hands that job to the caller. A dedup hit converges the transform and restores collision for
// the classes that want it, but skips the convergence write while the actor is under active
// PropPose drive, because the stream owns position while a prop is held.
//
// The fresh spawn is coop::prop_fresh_spawn::Materialize: a deferred spawn with setKey called
// BEFORE FinishSpawningActor, so Aprop_C.Init() cannot overwrite the Key with a new guid.
//
// No echo loop: this path spawns through engine::SpawnActor rather than UpropInventory_C.takeObj,
// so the takeObj POST observer that authors a send never fires. Its cross-TU calls into
// coop::remote_prop are IsActorUnderAnyDrive, RegisterPropMirror and KeyToWString.

namespace coop::net {
struct PropSpawnPayload;
struct WireClassName;
}  // namespace coop::net

namespace coop::remote_prop_spawn {

// Build a wide string from a wire class name (lossless for ASCII, and VOTV class names are
// ASCII). Shared so remote_prop::OnConvert can class-test a convert's pileClass -- the
// convert-before-spawn proxy form -- without duplicating it.
std::wstring ClassNameToWString(const coop::net::WireClassName& cn);

// Called from event_feed when a PropSpawn reliable message arrives. Game-thread only, since
// UFunction calls are; event_feed posts through game_thread::Post to satisfy that.
//
// `senderSlot` is the reliable header's senderPeerSlot, the host-relay logical origin, tagged onto
// the mirror so a disconnect can evict per slot. `localPlayer` (the live AmainPlayer_C*, possibly
// null) feeds the local-held guard: a prop the LOCAL player is grabbing is claimed and mirror-bound
// but never physics-reconciled or teleport-converged. `fromConvert` marks OnConvert's synthesized
// pile spawn: it disables the keyless-pile position bind and skips the eid dedup, because a
// convert-born pile has no local counterpart and must never bind to an unrelated one, and because
// the still-live old rendering of that eid must not converge the new one. `deferKerfur` (default
// true on the wire path) hands a prop-form kerfur the fuzzy match missed to the polled
// class-and-pose adoption rather than fresh-spawning a duplicate beside a still-loading twin.
// `skipBind` returns the fresh actor through `outSpawned` unbound, because the bind-model morph
// re-skins one eid in place and the rebind differs between a local element and a mirror.
void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot,
             void* localPlayer, bool fromConvert = false, bool deferKerfur = true,
             void** outSpawned = nullptr, bool skipBind = false);


}  // namespace coop::remote_prop_spawn
