// coop/props/prop_stick_sync.h -- the wall-attach component's stick and unstick (the cameras, the
// signs, the plasma TV), mirrored across peers.
//
// The stick is Blueprint-internal and invisible to a ProcessEvent observer except at one
// seam: its commit is reached only through a latent Delay resume, and the latent manager
// resumes ubergraphs through ProcessEvent, so an observer on
// ExecuteUbergraph_comp_wallAttachable sees every player-driven stick. A forceStick
// self-stick at load jumps to the commit locally and is not seen, which costs nothing:
// both peers run those themselves. The sticking peer broadcasts the frozen or static state
// with the commit pose; a receiver re-poses and dispatches the component's own forceStick,
// so the attach, the effect and the destroy binding stay the game's. Follows MTA's element-state
// RPCs (SET_ELEMENT_FROZEN, ATTACH_ELEMENTS in CElementRPCs.cpp): state messages, not event
// replay. docs/props.md carries the lane.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropStickStatePayload;
}  // namespace coop::net

// An unstick is state too, since not every unstick is a grab: a crowbar's pry frees a pryable and
// kicks it off the wall with nobody holding it. Every route reaches the component's unstick from
// script -- a grab's prelude, the pry's crowbarOpen, a fridge glow's knock -- so the script gate
// watches it. The unsticking peer broadcasts the unstick (flags 0) with the prop's pose and
// velocity once the body has freed it, and a receiver runs the component's own unstick with the
// tool and lets the copy go from there. A peer still loading gets neither message (the pre-world
// gate), so a join's snapshot row and its window correction carry the host's frozen and static, and
// ConvergeStuck makes the copy's stuck state the host's through the same two component verbs.

namespace coop::prop_stick_sync {

// Idempotent install, retried each net-pump tick until comp_wallAttachable_C loads. Two halves,
// each latched on its own, so a recook that loses one leaves the other working: the stick (the
// component class, ExecuteUbergraph_comp_wallAttachable, the component's `prop` field and
// forceStick, then the POST observer) and the unstick (unstick and its script-gate watch). Caches
// `session`.
void Install(coop::net::Session* session);

// Drain the stick commits and unsticks recorded since the last pass: verify (still live; still
// stuck for a stick, freed for an unstick) and broadcast as PropStickState. MUST run before
// local_streams::Tick within the pump pass (stick-before-release ordering). Cheap no-op when
// empty. Game thread (net pump).
void Tick();

// Receiver for PropStickState (wired in event_dispatch_entity): a stick, or an unstick when the
// flags are 0. `localPlayer` is this peer's player, whose own grab an unstick leaves alone. Game
// thread.
void OnStickState(const coop::net::PropStickStatePayload& payload, uint8_t senderPeerSlot,
                  void* localPlayer);

// A join's snapshot row or window correction makes this copy's stuck state the host's
// (`physFlags`, read off the host's live prop): a copy stuck here that is free on the host gets the
// component's unstick, with the tool, and a free copy the host's is stuck on gets the component's
// forceStick at the pose the caller has already put it at. Only for a wall-attach component's owner
// and only on a difference; the caller converges frozen and sleep after it. Game thread.
void ConvergeStuck(void* actor, uint8_t physFlags);

// True iff `actor` carries the wall-attach component in its comp_wallAttachable variable: the
// wall-attachable lineage and the plasma TV, which carries the component without the class. They
// are the props whose frozen and static a hold may clear. False until the component class
// resolves. Game thread.
bool IsWallAttachable(void* actor);

// The component's own unstick on this peer's copy, with the tool: the peer that freed the prop had
// it off the wall, by hand or by a pry, so the copy takes that outcome, and the tool only skips the
// check that fails a hand grab of a pried-on prop and shows its "Tool required" hint. Not mirrored
// back. False when the unstick half is not installed or the component does not resolve. Game
// thread.
bool ReplayUnstick(void* actor);

// Clear per-session state (the pending stick and unstick records). Net disconnect.
void OnDisconnect();

}  // namespace coop::prop_stick_sync
