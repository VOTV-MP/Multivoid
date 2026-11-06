// coop/creatures/walker_target.h -- the host's walking mannequin and what the host does with it.
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
// THE CLASS. Asked again only when the class set moved while it is null, dropped when its listing changes; the walkers
// alive at the resolve are claimed in one sweep, so one that minted before the class resolved is not missed.
// Coop layer; the subscriber is callable from any thread, the rest is game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::walker_target {

// The HOST's subscription to the prop lane's births, once for the process, from the session's first tick (coop/session/
// subsystems, InstallLoadWatchers). A client pays nothing. Game thread.
void Install(coop::net::Session& session);

// Per gameplay tick on the HOST: keep the walker's class (one listing compare), then claim what the queue holds. An empty
// queue costs one atomic read. Game thread.
void Tick(coop::net::Session& session);

// The session ended: the queue is emptied and the class forgotten. Game thread.
void OnDisconnect();

}  // namespace coop::walker_target
