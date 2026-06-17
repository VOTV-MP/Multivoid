// coop/remote_prop.h -- receiver-side held-prop driver (v4 wire, Stage 4).
//
// Mirrors RemotePlayer in shape but for physics-prop grab replication:
//   * On the FIRST PropPose for a new Key, look up the local Aprop_C* by
//     string match over GUObjectArray (prop_wrap::FindByKeyString), call
//     SetSimulatePhysics(false) on its StaticMesh so PhysX stops simulating
//     it, and cache the pointer for the grab duration.
//   * Per subsequent PropPose: SetActorLocation + SetActorRotation on the
//     cached actor.
//   * On PropRelease (or a >500 ms PropPose gap, treated as implicit
//     release): SetSimulatePhysics(true) + AddImpulse if the impulse vector
//     is non-zero (throw), clear the cache.
//
// Methodology-wise this matches MTA's ucSyncTimeContext-style ownership
// transfer: the sender (host) is authoritative for the held prop; the
// receiver kinematically displays it. When the host releases, the prop's
// physics resumes on every peer independently (same world state because
// Aprop_C.heavy / .Static etc. are content-driven, identical cross-peer).

#pragma once

#include "coop/element/element.h"
#include "coop/net/protocol.h"

#include <string>

namespace coop::net { class Session; }

namespace coop::remote_prop {

// Called every game-thread tick from the net pump. Drains the latest
// PropPose snapshot from `session` and applies it to the local prop
// instance (lookup-by-Key on first arrival, transform writes thereafter).
// Also handles the timeout-implicit-release if PropPose stream stops.
//
// Idempotent; runs only when the session is Connected.
void Tick(coop::net::Session& session);

// Handle an incoming RELIABLE PropRelease message. Receiver re-enables
// SimulatePhysics on the cached prop and, if the impulse vector is non-
// zero, AddImpulse for the throw + calls `Aprop_C.thrown(localPlayer)`
// to fire the natural throw-sound + particle-trail dispatch the prop's
// BP wires (RULE 1 -- same path NPCs and the local player use). Clears
// the cached pointer so the next PropPose has to re-resolve.
//
// `senderSlot` is the Registry slot of the peer that sent the release
// (from ReliableMessage::senderPeerSlot at the event_feed dispatch).
// Used to authoritatively identify WHICH slot's drive to clear --
// pre-fix `FindSlotByKey(payload.key)` linear-scanned by key, returning
// the first match, which would clear the wrong slot if two slots
// briefly held a prop with the same lastKey.
//
// `localPlayer` is passed so `prop.thrown` has a non-null Player param
// (the BP may null-check). Caller supplies the LOCAL mainPlayer_C ptr;
// the BP's "throw stats" attribution will credit local (semantically a
// minor inaccuracy in exchange for natural sound/effects).
void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer);

// v5: handle an incoming PropSpawn (peer dropped an inventory item into
// the world). MOVED (M-1 2026-05-29) to coop::remote_prop_spawn::OnSpawn.
// See coop/remote_prop_spawn.h for the public interface + design notes;
// this header retains the public accessors below that the spawn receiver
// calls back into for drive-state queries and mirror registration.

// Read-only predicate over the drive cache (g_drives). True iff any peer
// slot is currently kinematically driving `actor` via the PropPose
// stream. Used by remote_prop_spawn::OnSpawn to skip transform
// convergence on held props -- otherwise the convergence SetActor
// Location would stomp the active PropPose stream for one frame
// (visible teleport-pop until the next PropPose Tick).
bool IsActorUnderAnyDrive(void* actor);

// Register a wire-received Prop mirror Element at `eid` bound to
// `actor`. Idempotent for duplicate eids (silent no-op on duplicate).
// Skips eid==0 (legacy/unminted sender) and eid==kInvalidId (defensive
// reject). Implements the 5-step RegisterMirror pattern (alloc -> insert
// under MirrorManager mutex -> RegisterMirror -> rollback on failure).
// See [[feedback-registry-register-mirror-pattern]] for the contract.
// `senderSlot` is the originating peer slot (reliable header senderPeerSlot,
// the host-relay logical origin) -- tagged onto the mirror so a per-slot
// disconnect drains exactly this peer's mirrors (D1-7). Pass -1 if unknown.
void RegisterPropMirror(coop::element::ElementId eid,
                        void* actor,
                        const std::wstring& key,
                        const std::wstring& cls,
                        int senderSlot);

// Reverse lookup of the wire ElementId bound to `actor` in the prop MirrorManager, or kInvalidId if
// none. The forward map (prop_element_tracker::GetPropElementIdForActor) is the O(1) path for OWNED
// props; this is the MIRROR-side resolver (e.g. a CLIENT grabbing a host-owned chipPile) -- an O(n)
// Snapshot-walk reached ONLY on the rare grab edge AFTER the forward map missed, never a hot path.
// Game-thread only (Snapshot takes the MirrorManager lock). Used by trash_collect_sync's pile-grab
// observer to resolve a shared pile eid from the actor under the crosshair.
coop::element::ElementId ResolveMirrorEidByActor(void* actor);

// Extract null-terminated wstring from a WireKey. Shared utility used
// by both the drive subsystem (FindSlotByKey, OnRelease, OnDestroy)
// and the spawn receiver (OnSpawn). Public on this header because the
// spawn receiver lives in a separate TU after the M-1 2026-05-29 split.
std::wstring KeyToWString(const coop::net::WireKey& k);

// Drive-subsystem UFunction wrappers (SetSimulatePhysics + linear/
// angular velocity on UPrimitiveComponent). Used by the drive Tick
// path (PropPose stream) AND by the spawn receiver's fresh-spawn
// initial-physics application. Public so remote_prop_spawn doesn't
// have to duplicate the ~120 LOC of cached-UFunction resolution
// state these functions share with ResolveUFns(). Game-thread only
// (ProcessEvent contract).
void DriveSimulate(void* mesh, bool simulate);
void DriveSetLinearVelocity(void* mesh, float vx, float vy, float vz);
void DriveSetAngularVelocity(void* mesh, float wx, float wy, float wz);

// Force-release: called on disconnect / level unload to put any cached
// prop back into normal physics state. Safe to call when not holding.
void ForceRelease();

// Per-slot variant of ForceRelease for partial-disconnect (one peer
// drops mid-session while others stay connected). Without this, that
// peer's held prop stays kinematically frozen on remaining peers (no
// PropPose updates arrive to move it, no PropRelease re-enables
// physics). Safe to call when slot has no drive.
void OnDisconnectForSlot(int peerSlot);

// Handle an incoming PropDestroy. Resolves the key to a local
// AActor*, marks it as incoming-destroy (so the K2_DestroyActor observer
// doesn't echo), and calls K2_DestroyActor on it. Returns silently if no
// local actor with that key exists (the prop never replicated to us, or
// was already destroyed locally).
//
// 2026-05-25: `localPlayer` added for cross-peer destroy of a held prop.
// When client eats food that host is holding, host receives PropDestroy
// and we must call PHC.ReleaseComponent on host's mainPlayer.grabHandle
// BEFORE K2_DestroyActor -- otherwise UPhysicsHandleComponent::Tick
// Component reads GrabbedComponent (@+0xB0) as a dangling pointer the
// next frame and PhysX dereferences a freed body instance. The release
// path is gated on `mainPlayer.grabbing_actor == actor` so it no-ops
// for the common case where the destroyed prop isn't grabbed.
void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer);

// Clear every slot's kinematic-drive cache entry for `actor` (so nothing
// drives a destroyed actor next tick). Extracted from OnDestroy (Fork B 2e,
// 2026-06-10): the adoption sweep destroys actors through the same teardown
// contract -- one implementation. Game thread only; no-op for null.
void ClearAnyDriveFor(void* actor);

// v52: handle an incoming PropConvert -- the ATOMIC trash-clump ball->pile swap. Destroys the
// mirror BALL by payload.oldEid (the full OnDestroy eid teardown: drive-cache clear, PHC release,
// echo-suppressed K2_DestroyActor, UnregisterPropMirror) THEN spawns the authoritative pile by
// payload.newEid through the existing OnSpawn fresh-spawn path as a settled landed pile (+
// RegisterPropMirror). Both run in this one handler -> the ball vanishes the exact frame the pile
// appears: no lingering ball, no double pile (the two eids are distinct by construction). Returns
// the spawned pile actor so the caller can enroll it in the mirror-pile death-watch (for identity-
// based re-grab destroy), or null if the spawn failed. Game-thread only.
// RE: votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md.
void* OnConvert(const coop::net::PropConvertPayload& payload, void* localPlayer, int senderSlot);

// Echo-suppressed local destroy of an actor we own a copy of (MarkIncomingDestroy makes
// our K2_DestroyActor PRE observer skip the re-broadcast). Used by ForceRelease to tear
// down a null-mesh clump mirror on disconnect/teardown. (The former position-based OnSpawn
// "consume the co-located source pile" caller was RETIRED 2026-06-08 -- chipPiles are not
// co-located cross-peer; a re-grabbed pile now drops its mirror by IDENTITY via the
// trash_collect_sync mirror-pile death-watch -> PropDestroy(eid) / PropConvert, v52.)
// Game-thread only. No-op for null/dead. [[project-bug-trash-chippile-uaf-crash]]
void ConsumeLocalActor(void* actor);

}  // namespace coop::remote_prop
