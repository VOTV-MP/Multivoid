// coop/props/remote_prop.h -- the receiver-side held-prop driver, RemotePlayer's shape for
// physics-prop grab replication: on the first pose for a new key the local prop is looked up
// by key string, its mesh stops simulating so physics leaves it alone, and the pointer is
// cached for the grab; each later pose writes the location and rotation on the cached actor;
// a release (or a pose gap over half a second, an implicit release) re-enables simulation,
// applies the launch velocity and clears the cache. The MTA-style ownership transfer: the
// sender is authoritative for the held prop and the receiver displays it kinematically; on
// release the prop's physics resumes on every peer independently, from the same world state,
// since the prop's content-driven properties are identical cross-peer.

#pragma once

#include "coop/element/element.h"
#include "coop/net/protocol.h"

#include <string>

namespace coop::net { class Session; }

namespace coop::remote_prop {

// Called every game-thread tick from the pump. Drains the latest pose snapshot from the
// session and applies it to the local prop (a lookup by key on first arrival, transform
// writes after), and handles the timeout-implicit release when the stream stops. Idempotent;
// runs only while the session is connected.
void Tick(coop::net::Session& session);

// Handle an incoming reliable release: the receiver re-enables simulation on the cached prop,
// applies the launch velocity, and fires the prop's own thrown event when the launch speed
// exceeds the threshold, so a passive drop stays silent and a real throw plays the game's own
// sound and trail. Clears the cached pointer, so the next pose re-resolves. `senderSlot` is
// the sender's slot from the reliable header, which identifies which slot's drive to clear (a
// scan by key returned the first match and could clear the wrong slot when two slots briefly
// held a prop with the same key). `localPlayer` gives the thrown event a non-null player; the
// game's throw statistics credit the local player, a minor inaccuracy for the natural
// effects.
void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer);

// An incoming spawn (a peer dropped an inventory item into the world) is handled in
// remote_prop_spawn; this header keeps the accessors the spawn receiver calls back into for
// drive-state queries and mirror registration.

// A read-only predicate over the drive cache: true if any peer slot is kinematically driving
// `actor` through the pose stream. The spawn receiver uses it to skip transform convergence
// on held props; otherwise the convergence write would stomp the active pose stream for one
// frame, a visible pop.
bool IsActorUnderAnyDrive(void* actor);

// Register a wire-received prop mirror Element at `eid` bound to `actor`. Idempotent for
// duplicate eids (a silent no-op); skips an eid of 0 (an unminted sender) and the invalid id.
// The register-mirror pattern: allocate, insert under the manager mutex, register, roll back
// on failure. `senderSlot` is the originating peer slot from the reliable header, tagged onto
// the mirror so a per-slot disconnect drains exactly that peer's mirrors; -1 if unknown.
// `rebindInPlace`, the morph sites only: when the eid already exists bound to a different
// live actor the default rejects the duplicate, since a different live actor keeps the eid;
// the bind-model morph re-skins one eid across pile, clump and pile (one actor at a time, the
// old destroyed right after), so a morph rebind must re-point the existing mirror Element
// onto the new rendering instead. Only for mirror eids; a host's own local element rebinds
// through the element tracker, which also fixes the forward map.
void RegisterPropMirror(coop::element::ElementId eid,
                        void* actor,
                        const std::wstring& key,
                        const std::wstring& cls,
                        int senderSlot,
                        bool rebindInPlace = false);

// The reverse lookup of the wire eid bound to `actor` in the prop mirror manager, or the
// invalid id. The element tracker's forward map is the constant-time path for owned props;
// this is the mirror-side resolver (a client grabbing a host-owned chipPile), a linear
// snapshot walk reached only on the rare grab edge after the forward map missed. Game thread
// only, since the snapshot takes the manager lock. `wireMirrorOnly`, the identity-steal gate:
// the prop manager is the one manager that mixes locally allocated elements (every keyed
// interactable enters at the census walk) with wire mirrors. The default matches either
// kind; true matches only a wire-mirror row, for the caller that must distinguish an
// established cross-peer identity from a save-loaded local awaiting adjudication (the spawn
// receiver's fuzzy steal gate).
coop::element::ElementId ResolveMirrorEidByActor(void* actor, bool wireMirrorOnly = false);

// Extract the string from a wire key. Shared by the drive (the slot lookup, the release, the
// destroy) and the spawn receiver.
std::wstring KeyToWString(const coop::net::WireKey& k);

// A Prop element id to its live actor, or null: the registry's O(1) row, liveness by the cached
// object-array index. The clump is identified this way, its key never sticks; the driven-prop
// receiver resolves by it first, since a joiner's keyed copy adopts the host's eid at the bind.
// Game thread.
void* ResolveLiveActorByEid(uint32_t eid);

// The drive's UFunction wrappers (simulate physics, linear and angular velocity on a
// primitive component), used by the drive tick and by the spawn receiver's initial physics;
// public so the spawn receiver does not duplicate the cached resolution state. Game thread
// only.
void DriveSimulate(void* mesh, bool simulate);
void DriveSetLinearVelocity(void* mesh, float vx, float vy, float vz);
void DriveSetAngularVelocity(void* mesh, float wx, float wy, float wz);

// Force-release: called at disconnect or a level unload to put any cached prop back into its
// normal physics state. Safe when not holding.
void ForceRelease();

// The per-slot variant for a partial disconnect, one peer dropping mid-session while others
// stay: without it that peer's held prop stays kinematically frozen on the remaining peers,
// with no poses to move it and no release to re-enable physics. Safe when the slot has no
// drive.
void OnDisconnectForSlot(int peerSlot);

// Handle an incoming destroy. Resolves the key to a local actor, marks it as an incoming
// destroy so the destroy observer does not echo, and destroys it. Returns silently if no
// local actor carries the key (the prop never replicated to us, or was already destroyed).
// `localPlayer` is for the cross-peer destroy of a held prop: when a client eats food the host
// is holding, the host must release its physics handle before the destroy, or the handle's
// tick reads the grabbed component as a dangling pointer next frame and physics dereferences
// a freed body; the release is gated on the player's grabbing actor being this actor, so it
// no-ops in the common case.
void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer);

// The deferred re-apply of a destroy that arrived before its target loaded, the
// destroy-before-load race. Called only by the drain-edge order owner at the quiescence sweep,
// after the bind, so an out-of-order destroy reconciles instead of leaking a duplicate.
// Resolves the local player itself. True if the now-loaded actor was destroyed (erase from
// the pending queue); false means still not loaded, keep queued. Never re-arms. Game thread.
bool TryApplyDestroy(const coop::net::PropDestroyPayload& payload);

// Clear every slot's kinematic-drive cache entry for `actor`, so nothing drives a destroyed
// actor next tick. The adoption sweep destroys actors through the same teardown contract, one
// implementation. Game thread only; a no-op for null.
void ClearAnyDriveFor(void* actor);

// Handle an incoming convert, the bind-model pile morph: the re-skin of eid E in place (the
// old and new eids equal). Resolves this peer's current rendering of E (a pile or a clump),
// spawns the new rendering bound to the same E at the payload transform (to-clump: a
// kinematic clump the held-pose stream drives; to-pile: a settled, grabbable pile), rebinds E
// onto it (a local element through the tracker, a mirror through the in-place rebind), then
// echo-destroys the old rendering: spawn, rebind, then destroy, so E never resolves to a dead
// actor. Idempotent: a convert whose target rendering already matches (an echo, or a
// grab-race loser's convert) is a no-op. Returns the new rendering, or null on a spawn
// failure. The host applies a client's convert against its own local element, host authority
// through the host-minted eid, no request or relay. Game thread only.
void* OnConvert(const coop::net::PropConvertPayload& payload, void* localPlayer, int senderSlot);

// The echo-suppressed local destroy of an actor we own a copy of: the incoming-destroy mark
// makes our destroy observer skip the re-broadcast. Used by ForceRelease to tear down a
// null-mesh clump mirror at teardown. Game thread only; a no-op for null or dead.
void ConsumeLocalActor(void* actor);

}  // namespace coop::remote_prop
