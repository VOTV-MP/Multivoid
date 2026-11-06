// coop/props/prop_settle.h -- where a released prop came to rest, from the peer that let it go. Overview:
// docs/props.md.
//
// A release hands every copy the same transform and velocity, and each peer's own physics takes it from
// there, so the copies came to rest apart: centimetres on a floor, a metre when one stayed on a shelf the
// other rolled off. The releaser keeps watching its copy after the release, and once the copy has not
// moved past the drive's send epsilons for the drive's rest time it sends where the copy rests
// (PropSettle). Every other peer whose copy no hold, drive or park moves puts its copy there, with the
// releaser's frozen and sleep and no velocity. MTA leaves an unoccupied vehicle with its last syncer until
// it stops (CUnoccupiedVehicleSync.cpp); here the releaser is that syncer at the end, and each copy flies
// by its own physics on the way, so a drop looks the same on every peer until the copies land. A throw is
// not watched: its thrower and the host stream it to rest.
//
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WireKey;
struct PropSettlePayload;
}  // namespace coop::net

namespace coop::prop_settle {

// HOLDER, at the release edge of hold `holdGen` of `actor` under `key`: watch the copy until it rests. A
// prop the hold claim does not arbitrate, one frozen or static at the edge, and a second watch past the
// table's bound are not watched. Game thread.
void Watch(void* actor, uint16_t holdGen, const coop::net::WireKey& key);

// Per gameplay tick: each watched copy that rested goes out as a PropSettle; one a hand, a drive or a park
// took, one that died and one still moving at the bound are dropped. Game thread.
void Tick(coop::net::Session& session);

// RECEIVER: the peer in `senderSlot` says where its released prop rests. Validated at the router.
void OnSettle(int senderSlot, const coop::net::PropSettlePayload& p);

// Session end: the watches and the counters. Game thread.
void OnDisconnect();

}  // namespace coop::prop_settle
