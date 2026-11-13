// coop/player/hand_item_applied.h -- what hand_item.cpp (the owner's announce, the receipt, the mirrors) and
// hand_item_applied.cpp (the host's applied hand state of each slot, and the pace it is applied at) share. Src-local.

#pragma once

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::hand_item::detail {

// Wire caps: class names and item names are short ASCII, one length byte each.
constexpr size_t kMaxStr = 63;
// The largest HandItem: slot, has, three length-prefixed strings at the cap, the hold transform.
static_assert(2 + 3 * (1 + kMaxStr) + 6 * sizeof(float) <= static_cast<size_t>(coop::net::kMaxReliablePayload),
              "a HandItem at its string caps must fit one reliable datagram");

// The fallback placement until the owner's first measure lands (coop/player/hand_item_pose measures
// and drives the view-space hold): camera-front constants.
constexpr float kFallbackRelPos[3] = {45.f, 10.f, -40.f};  // fwd/right/up cm

// Per-slot wire state: what each peer's hand holds.
struct SlotHand {
    bool has = false;
    std::wstring cls;   // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;  // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
    std::wstring item;  // the hold slot's item name, which the game's item tables are keyed by
    std::wstring lastCls;  // the class held last: `cls` while it holds one, the one before once empty
    uint32_t holdGen = 0;  // host: the hold announces applied for this occupant (HoldGeneration)
    // The measured view-relative hold transform, owner-authored and wire-carried.
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};  // item rotator {pitch,yaw,roll} in the view frame
    bool  haveRel = false;              // owner measured (vs the fallback defaults)
};
extern SlotHand g_hands[coop::players::kMaxPeers];  // GT-only

// The session, cached by TickOwner for the entries that only receive a slot. Game thread only.
extern coop::net::Session* g_session;

// The payload: slot, has, a length-prefixed class, name and item, and when held the measured view-relative
// position and rotation.
std::vector<uint8_t> BuildPayload(uint8_t slot, const SlotHand& h);

// A hand state to the peers it concerns: a host to every other ready client, a client to the host.
void SendState(coop::net::Session& session, uint8_t slot, const SlotHand& h);

// Host, a client's changed state `in` for `slot`, kept newest and applied at the lane's pace (coop/net/latest_wins.h),
// the newest kept past it. True when the state equals the applied one, which nothing owed survives (a withdrawn state
// is an edge for the subscribers too); false when it differs, and `Offer` then keeps it.
bool Settles(uint8_t slot, const SlotHand& in);
void Offer(coop::net::Session& session, uint8_t slot, const SlotHand& in);

}  // namespace coop::hand_item::detail
