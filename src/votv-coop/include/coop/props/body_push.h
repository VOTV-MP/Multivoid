// coop/props/body_push.h -- a prop a player walks into rests alike on every peer.
//
// A prop at rest is the host's (docs/props.md), and each peer's copy keeps its own physics. A player's body that walks
// into one knocks every copy, since the player's puppet stands in every world, but each copy settles the knock by its
// own physics and nothing brings them back together: a woodchipper a client walked into came to rest up to 9 cm and
// 12 degrees from the host's. This is MTA's push (CClientGame.cpp:4712-4733): the peer whose own player set the prop
// moving syncs it. Each peer keeps the keyed props within 4 m of its player (a list refreshed once a second, read from
// memory), and ten times a second compares their poses; one that moved more than 3 cm or turned more than 3 degrees,
// with this peer's player within 1.5 m and nearer to it than any other player, nobody holding or driving it, and no
// stream parking it here, is this peer's push: the host coasts it on the driven-prop channel until it rests
// (prop_drive_host::Coast), a client claims it as its syncer (prop_syncer::Claim, reason BodyPush), and its pose reaches
// every peer until its rest End. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::body_push {

// The nearby list and the pose compare, from the driven-prop pump. Game thread.
void Tick(coop::net::Session& s, bool isHost);

// The session ended: the counts are said and the list forgotten.
void OnDisconnect();

}  // namespace coop::props::body_push
