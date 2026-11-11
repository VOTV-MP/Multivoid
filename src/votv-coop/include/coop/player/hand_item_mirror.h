// coop/player/hand_item_mirror.h -- the display mirrors of the hand-item axis (coop/player/hand_item.h): one per
// peer slot and hand, built from the slot's announced state on that slot's puppet -- the main hand's held in the
// puppet's view (coop/player/hand_item_pose.h), the off hand's attached to the puppet's own `weapon_off`. Internal
// to the axis: the state it reads is the lane's (HandOf). Game thread only.

#pragma once

#include "coop/player/hand_item_wire.h"

#include <cstdint>
#include <string>

namespace coop { class RemotePlayer; }

namespace coop::hand_item {

using Hand = coop::hand_item_wire::Hand;

// The fallback placement until the owner's first measure lands (coop/player/hand_item_pose measures
// and drives the view-space hold): camera-front constants.
inline constexpr float kFallbackRelPos[3] = {45.f, 10.f, -40.f};  // fwd/right/up cm

// Per-slot wire state: what each peer's hand holds, one per hand. The off hand's carries no transform.
struct SlotHand {
    bool has = false;
    std::wstring cls;   // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;  // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
    std::wstring item;  // the hand's item name, which the game's item tables are keyed by
    std::wstring lastCls;  // the class held last: `cls` while it holds one, the one before once empty
    // The measured view-relative hold transform, owner-authored and wire-carried.
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};  // item rotator {pitch,yaw,roll} in the view frame
    bool  haveRel = false;              // owner measured (vs the fallback defaults)
};

// The lane's state of `slot`'s `hand` (coop/player/hand_item.cpp). slot < kMaxPeers.
SlotHand& HandOf(Hand hand, uint8_t slot);

// "main" or "off", for the log.
const char* HandName(Hand hand);

// The prop name field's offset, resolved once (the field lives on the prop base class, and every hotbar item is a
// descendant); -1 until it resolves.
int32_t PropNameOffset();

// What a class name off the wire is to this lane. The owner announces its hand actors only when they
// are prop descendants (local_streams), so a hand item is a prop; and not of the two prop families the
// coop layer never puts on the wire (prop_lifecycle): a per-player state actor, of which each peer owns
// exactly one, and a wire-suppressed variant, which a client's own Init destroys. The receipt and the
// spawn both ask it, so the two cannot drift. The index finds a class whatever the letter case of the
// name, and the family predicates compare without case too. Without the prop base this peer cannot
// judge, and says so rather than refusing. `outOwn`, when asked, gets a hand item's class as the engine
// spells it.
enum class HandClass { Hand, NotHand, Unresolved };
HandClass ResolveHandClass(const std::wstring& name, void** outCls, std::wstring* outOwn);

namespace mirror {

// `slot`'s `hand` mirror kept to what the slot's state wants: destroyed when the hand is empty or the puppet is
// gone (`puppetActor` null), spawned again when the item or the puppet changes, and the main hand's re-commanded
// to the view hold.
void Tick(Hand hand, uint8_t slot, coop::RemotePlayer* pup, void* puppetActor);

// Destroy `slot`'s `hand` mirror, silently to the peers, and forget it.
void Destroy(Hand hand, uint8_t slot, const char* why);

// `slot`'s live `hand` mirror, or null.
void* Live(Hand hand, uint8_t slot);

// Forget what `slot`'s mirrors waited on (a session reset or the occupant left).
void ForgetSlot(uint8_t slot);

}  // namespace mirror

}  // namespace coop::hand_item
