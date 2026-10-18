// coop/player/puppet_identity.h -- a remote player's puppet is never "this machine's player".
//
// A puppet is an unpossessed mainPlayer_C, so a Blueprint that casts "the actor that touched me" to the player
// passes it, and then does what only this machine's own player earns: saves, quits, travels, leaves a dream,
// turns the player's camera, plays a 2D sound, shows a flashbang. Each such path is refused at its own seam when
// the toucher is a puppet body (players::Registry::IsPuppetBody), and runs unchanged for this machine's player
// and in single player. A stub that tests no toucher is cancelled whole; a chain that does other work the host
// needs is cut at the one callee that is this machine's alone (a scope mark read by a later seam); a scan whose
// result the Blueprint reads has the puppet removed from the result. Every row runs on every machine: the host
// refuses a client's puppet, a client the host's or another client's puppet touching its copy. Game thread.
//
// Divergence from MTA, whose handlers test a positive local identity (CClientGame.cpp:4481, IsLocalPlayer) and
// deliver a pickup's personal effect to the toucher's own client (Server CPickup.cpp:435-438): here a deny-list
// of puppet bodies, and no delivery yet -- a refused effect reaches nobody, and the world half of rows 1-5's
// consumed creature is restored by the per-player delivery, not by un-refusing here.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::puppet_identity {

// Register the stub watches and the puppet-count listener once, and install each native seam as soon as its
// class resolves; keep the session, whose role row 3 reads. Retried every pump tick; idempotent.
void Install(coop::net::Session* session);

// Settle the watches' registration and say once how many are live, then re-resolve the per-world pointers of
// each of the module's own classes that was loaded, unloaded or loaded again. Game thread, per frame.
void Tick();

// The session's caches. Session end.
void OnDisconnect();

// How many of the module's name watches were live when they settled (the count the settle line says): 0 until
// then, and not recounted after.
int WatchesLive();

// A row's name and how many times it refused a puppet since the process started. The drills' read.
struct RowCounter {
    const char* row;
    uint32_t    refused;
};
const RowCounter* Counters(int& n);

}  // namespace coop::puppet_identity
