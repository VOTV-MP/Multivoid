// coop/props/prop_hold_claim.h -- one hold per keyed prop: the host keeps the first hold it sees and refuses
// a second. Overview: docs/props.md.
//
// A grab is the game's own and runs on the grabbing machine before any message leaves it, so two players
// can take one prop inside one round trip. Two halves keep one holder. A peer whose copy another hold
// already drives lets its own player's grab go at the new-held edge, with the deny click and a line naming
// the holder. And the host judges every remote hold before its drive starts: a hold of a prop its own
// player holds, or that another client's hold drives, is refused to every client (PropHoldRefused) --
// MTA's one syncer per unoccupied vehicle, the server keeping the first. The refused holder lets go and
// sends no release, and every peer drops that hold's drive; a client that meets a second hold waits for
// the host's word instead of judging. Keyed Aprop_C props only: a dragged swinger and a trash clump keep
// their own rules, and a hook's pull yields to a hand.
//
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WireKey;
struct PropHoldRefusedPayload;
}  // namespace coop::net

namespace coop::prop_hold_claim {

// Cache the session. Game thread.
void Install(coop::net::Session* session);

// True for a prop whose holds the host arbitrates: a live keyed Aprop_C that is no swinger and no trash body.
bool IsArbitrated(void* actor);

// HOLDER, at the new-held edge of `actor`: false when another peer's hold drives this copy. The local grab is
// let go here, and the player hears the deny click and reads who holds it; the caller streams nothing for it.
bool AdmitLocalGrab(void* localPlayer, void* actor);

// RECEIVER, before hold `holdGen` of the peer in `slot` starts its drive on `actor`: true when it may start.
// HOST: a hold of a prop its own player holds, or that another slot's hold drives, is closed here and refused to
// every client. CLIENT: false while this peer's player or another slot holds the prop, until the host's refusal
// settles which hold stays.
bool AdmitRemoteHold(int slot, uint16_t holdGen, const coop::net::WireKey& key, void* actor);

// CLIENT: the host refused a hold. This peer's own lets the prop go and sends no release; another slot's closes
// and its drive ends. Validated at the router.
void OnRefused(const coop::net::PropHoldRefusedPayload& p);

// HOLDER: the host refused this peer's hold `holdGen`, so its release is not sent.
bool IsOwnHoldRefused(uint16_t holdGen);

// Session end: the refusal records, the notice cooldown and the counters. Game thread.
void OnDisconnect();

}  // namespace coop::prop_hold_claim
