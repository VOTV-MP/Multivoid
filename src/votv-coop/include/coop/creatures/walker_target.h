// coop/creatures/walker_target.h -- the host's walking mannequin: its claim into the drive stream, and the player it hunts.
//
// The walking mannequin (prop_wMannequin_C) is a prop that walks by its own Blueprint, with no hand and no verb. It crosses as
// a prop under its key, and its copies are moved by the host's drive stream (coop/props/prop_drive_host), which holds a
// row only for a prop a caller claimed. This module is the claim: at the host's mint of the walker's eid (the prop lane's
// MarkPropElement, which every route to an eid passes: the Init body, the census, the seed, the re-seed) the walker is
// claimed, once, and the row lives until the actor dies. A walker frozen by its own gaze stops changing pose, the stream
// rests, and the copy stops dead.

// THE QUEUE. MarkPropElement can run off the game thread, so the subscriber does the one thing safe there: it rejects an
// actor whose class is not the walker's by a pointer compare, then pushes (actor, eid) into a fixed queue under a leaf mutex
// (no allocation; an overflow is said once). The game-thread tick drains it and claims. A queued actor that died or lost its
// eid before the drain is skipped by the registry's reverse.
// THE CLASS. While null, asked again on a class-set move and then each tick for the load window (class_reask::Latch), dropped
// when its listing changes; the walkers alive at the resolve are claimed in one sweep, so none minted before it is missed.
// Coop layer; the subscriber is callable from any thread, the rest is game thread.

#pragma once

#include "coop/creatures/walker_target_rule.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::walker_target {

// THE HUNT. Its Blueprint walks to player 0's camera manager and nothing in it names another player, so a claimed walker
// holds one TARGET slot (0 the host, kNoTarget none) and the host answers the Blueprint's read of that camera manager with
// the target's body. It is a post hook on GameplayStatics.GetPlayerCameraManager, armed only while a walker is held, that
// first compares the calling frame's function with the walker's ubergraph (one pointer compare for every other caller) and
// then acts at the walker's pinned calls of the native (found in the bytecode at the class resolve, two of them). At such a
// call a walker with no target takes the nearest living player within the Blueprint's own range, the host when none is in
// range, so the Blueprint's own out-of-range branch waits. It KEEPS that target until it is lost: its slot left, a client's
// puppet is not alive or has no body when read here (the call is the event: no death edge exists), or it lies beyond the
// range, which is the Blueprint's own test of the same read. A lost target is chosen again at that same read. A walker is
// tracked from its claim, so the call that begins its life, before its eid exists, is the Blueprint's own (the host). The choice
// itself is a pure function (walker_target_rule.h) with a boot selftest.
// The walker is served its target through served_player (the Blueprint's getMainPlayer reads, its ragdoll test and its hit),
// the record following every change of the target through the subscription below.

// The HOST's subscription to the prop lane's births, once for the process, from the session's first tick (coop/session/
// subsystems, InstallLoadWatchers). A client pays nothing. Game thread.
void Install(coop::net::Session& session);

// Per gameplay tick on the HOST: keep the walker's class (one listing compare), then claim what the queue holds. An empty
// queue costs one atomic read. Game thread.
void Tick(coop::net::Session& session);

// The body of the client at `slot` as the hunt reads it: its puppet's actor once the puppet has a pose, else null. Game thread.
void* ClientBody(uint8_t slot);

// The slot `walker` hunts, kNoTarget for a walker not held or not yet given one. Game thread.
uint8_t TargetOf(void* walker);

// Told whenever a held walker's target changes, and when the walker ends or its target's slot leaves (`slot` is then
// kNoTarget): `walker` may be dying, so compare its pointer and never read through it. At most six, the module's own log
// line being one; false when full or when `fn` is already subscribed. Game thread, in the order of the changes.
using TargetChangedFn = void (*)(void* walker, uint8_t slot);
bool SubscribeTargetChanged(TargetChangedFn fn);

// Told with the walker's pinned event graph while at least one walker is held, and with null when that stops (no walker held,
// the graph unpinned or the session ended): a consumer's own hook on a native the walker's graph calls is armed exactly then,
// and pins its calls in `graph`. A graph that changes is told as the new graph. A subscriber added while a walker is held is told
// at once. At most six; false when full or when `fn` is already subscribed. Game thread.
using GraphFn = void (*)(void* graph);
bool SubscribeGraph(GraphFn fn);

// Every held walker that is still alive, with the slot it hunts (kNoTarget before its first read), in no order. Game thread.
using HeldFn = void (*)(void* ctx, void* walker, uint8_t target);
void ForEachHeld(HeldFn fn, void* ctx);

// A peer left: every walker that hunted `slot` has no target until its next read chooses one. Game thread.
void OnPeerLeft(uint8_t slot);

// The session ended: the queue is emptied, the walkers forgotten, the hook disarmed and the class forgotten. Game thread.
void OnDisconnect();

}  // namespace coop::walker_target
