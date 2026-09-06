// coop/props/prop_stick_sync.h -- the wall-attachable surface stick (the cameras),
// mirrored across peers.
//
// The stick is Blueprint-internal and invisible to a ProcessEvent observer except at one
// seam: its commit is reached only through a latent Delay resume, and the latent manager
// resumes ubergraphs through ProcessEvent, so an observer on
// ExecuteUbergraph_comp_wallAttachable sees every player-driven stick. A forceStick
// self-stick at load jumps to the commit locally and is not seen, which costs nothing:
// both peers run those themselves. The sticking peer broadcasts the frozen or static state
// with the commit pose; a receiver re-poses and dispatches the component's own forceStick,
// so the attach, the effect and the destroy binding stay the game's. Unsticking carries no
// message -- an unstick is a grab, and a grab already streams poses.
//
// Follows MTA's element-state RPCs (SET_ELEMENT_FROZEN, ATTACH_ELEMENTS in
// CElementRPCs.cpp): state messages, not event replay. docs/props.md carries the lane.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropStickStatePayload;
}  // namespace coop::net

namespace coop::prop_stick_sync {

// Idempotent install, retried each net-pump tick until comp_wallAttachable_C loads:
// resolves the component class, ExecuteUbergraph_comp_wallAttachable, the comp's `prop`
// field, prop_wallAttachable_C with its comp_wallAttachable field, and forceStick, then
// registers the POST observer. Caches `session`.
void Install(coop::net::Session* session);

// Drain the stick commits recorded by the observer since the last pass:
// verify (still live + frozen/static) and broadcast as PropStickState. MUST
// run before local_streams::Tick within the pump pass (stick-before-release
// ordering). Cheap no-op when empty. Game thread (net pump).
void Tick();

// Receiver for PropStickState (wired in event_dispatch_entity). Game thread.
void OnStickState(const coop::net::PropStickStatePayload& payload,
                  uint8_t senderPeerSlot);

// True iff `actor` is an Aprop_wallAttachable_C descendant (the only lineage
// whose frozen/static a pose stream may legitimately clear). False until the
// class resolves. Cheap SuperStruct walk; any thread.
bool IsWallAttachable(void* actor);

// Clear a stuck wall-attachable for an incoming kinematic drive: frozen=false, static=false
// and SetActorSimulatePhysics(true), which is the SP unstick shape with the simulate
// recompute applied directly -- enabling simulate also detaches an attached root in
// UE4.27, which the Blueprint's own unstick relies on. No init() is dispatched anywhere in
// this module; the .cpp says why. Returns true if the actor WAS stuck and is now clear.
// The caller (remote_prop) gates the call on IsWallAttachable and its sustained-stream
// check. Game thread.
bool UnstickForDrive(void* actor);

// Clear per-session state (the commit-pending list). Net disconnect.
void OnDisconnect();

}  // namespace coop::prop_stick_sync
