// coop/held_clump_sync.h -- mirror a HELD trash clump via the MTA attach model.
//
// The trash ball / chipPile-clump (prop_garbageClump_C) is the actual object a
// player carries after grabbing trash. It is NON-Aprop_C and NON-KEYABLE: the
// autonomous clump test (commit 33e7f25) proved setKey doesn't stick on it
// (GetKey re-reads None after a force-mint), so the keyed PropSpawn/PropPose
// pipeline that mirrors the mannequin CANNOT carry it -- there is no stable key
// to match a per-tick world-pose stream against. Four key-based attempts
// (2a/v1/v2/v3) all failed for exactly this reason; 2a additionally CRASHED
// (use-after-free, the morphing clump read as a stable physics prop).
// [[project-bug-trash-chippile-uaf-crash]]
//
// The root-cause-correct fix is the MTA attach model (the same primitive as the
// puppet-ragdoll pelvis attach): the holder broadcasts two reliable EDGES and
// each peer mirrors the clump by SPAWNING a copy and K2_AttachToComponent-ing it
// to the holder puppet's HAND bone. The puppet hand is already pose-synced, so
// the attached clump follows the hand for free -- no per-tick clump stream, no
// key. On release the mirror detaches + re-enables physics + inherits the
// holder's throw velocity ("physics like the mannequin", the user's RULE-1
// requirement -- not the v3 kinematic crutch).
//
// CRASH-SAFETY: nothing here touches the clump's mesh (ue_wrap::prop::GetStaticMesh
// still returns null for non-Aprop_C, so the reverted-2a UAF is structurally
// unreachable). The SENDER only derefs the LIVE current held actor on the edge
// (never a cached pointer across ticks). The RECEIVER works on OUR spawned mirror
// (a stable actor that is never grabbed, so it never morphs/self-frees on
// interaction) and IsLiveByIndex-gates every access.
//
// 2-peer host<->client is fully covered (direct send). client<->client (3-peer)
// needs the host to relay HeldClumpGrab/Release (like the T2-2 pose relay); that
// is a follow-up (the kind is not in the relay whitelist yet).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HeldClumpGrabPayload;
struct HeldClumpReleasePayload;
}  // namespace coop::net

namespace coop::held_clump_sync {

// ---- SENDER (net_pump held-edge, game thread) ----

// True iff `heldActor` is a non-Aprop_C keyed interactable -- a trash clump that
// must use this attach path instead of the keyed prop pipeline. (Aprop_C keyed
// items still ride trash_collect_sync / PropPose.) Cheap; null-safe.
bool IsAttachClump(void* heldActor);

// Grab edge: broadcast HeldClumpGrab for the LIVE `clumpActor` so peers spawn +
// attach a mirror to this holder's puppet hand. Returns true if sent. Game thread.
bool SendGrab(void* clumpActor, coop::net::Session* session);

// Release edge: read `clumpActor`'s root velocity (if still live) + broadcast
// HeldClumpRelease so peers detach + free-fall the mirror with the throw energy.
// `clumpActor` may already be dead (consumed on release) -> zero velocity is sent.
// Game thread.
void SendRelease(void* clumpActor, coop::net::Session* session);

// ---- RECEIVER (event_feed GT::Post, game thread) ----

// Spawn a mirror of the grab payload's class + attach it to `puppetActor`'s hand
// (holder slot `peerSlot`). Replaces any prior held mirror for the slot. No-op if
// `puppetActor` is null/dead (the grab is dropped -- the next grab works once the
// puppet exists; held clumps are transient so a one-frame race is harmless).
void ApplyGrab(uint8_t peerSlot, void* puppetActor, const coop::net::HeldClumpGrabPayload& p);

// Detach the slot's held mirror, re-enable its physics, apply the throw velocity.
// The released mirror enters a bounded ring for eventual cleanup (the clump has no
// key/eid the existing PropDestroy could carry, so exact despawn-sync is a
// follow-up; the ring caps accumulation).
void ApplyRelease(uint8_t peerSlot, const coop::net::HeldClumpReleasePayload& p);

// ---- Lifecycle cleanup (game thread) ----

// Destroy the departing slot's held mirror (its puppet is going away).
void OnDisconnectForSlot(int peerSlot);
// Destroy ALL mirrors (every slot's held + the released ring) on full teardown.
void OnDisconnect();

}  // namespace coop::held_clump_sync
