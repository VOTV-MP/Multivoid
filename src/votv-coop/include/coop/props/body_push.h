// coop/props/body_push.h -- the player who bumps a resting prop syncs it, and only that player's own body moves it.
//
// A player's body that walks into a prop used to knock every copy, since its puppet stands in every world, and each copy
// settled the knock by its own physics: a woodchipper a client walked into came to rest up to 9 cm and 12 degrees from the
// host's. MTA's answer is that the player who bumps an unsynced element becomes its syncer (the client's push packet,
// CClientGame.cpp:4712-4733; the SERVER's judge and override, CUnoccupiedVehicleSync.cpp:476-509, not the client tree's file
// of that name): one physics source per push, and a remote ped is never a physics author. So a push is read from the local
// player's own body only, and a prop is syncd by the pusher's own peer.
// A CLIENT's push is a PropSyncClaim (reason BodyPush) the host judges like a throw's election; the claim-holder's copy
// streams while every other copy, the host's included, is driven. The HOST's own body makes the host coast a prop no row
// streams (prop_drive_host::Coast, cause BodyPush). Each peer keeps the props within a runner's second of its own player's
// body (a list refreshed once a second, nearest 64) and ten times a second compares their poses; a move over 3 cm or
// 3 degrees, more sideways than down, with the body within 1.5 m, is a push once prop_syncer or prop_drive_host judges the
// prop (a keyed one, in no hand, on no driven vehicle, with no row of its own). Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::body_push {

// The nearby list and the pose compare, from the driven-prop pump, on both roles. The HOST's list also takes the props
// near every other player's puppet, for the settle watch: a move there, or a fall anywhere in the list, with no hand, row
// or host body on the prop (its support taken away, something knocked it) is a Settle coast, so every copy follows the
// host's. A move a client's own body makes near its puppet is left to that client's push claim. The robots the host's
// brain drives (a Kerfus that is on, every Omega) are listed round as the puppets are, and a robot nearer than a puppet
// makes a move the host's settle. A CLIENT's copy that moves with its body away, or a robot nearer, unparked, is a
// prop_rest_sync::NoteLocalMove.
void Tick(coop::net::Session& s, bool isHost);

// This peer's own write has just put `actor` in a new place (an End, a correction snap): the next read takes that pose as
// the anchor and reads no push from it. A prop not in the list is ignored. Game thread.
void Rebase(void* actor);

// The session ended: the count is said and the list forgotten.
void OnDisconnect();

}  // namespace coop::props::body_push
