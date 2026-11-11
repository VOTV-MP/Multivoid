// coop/player/hand_item_wire.h -- the HandItem payload (coop/player/hand_item.h is the axis): what one hand of a
// player shows, packed and parsed. Pure byte work, no engine access.
//
// [u8 slot][u8 hand][u8 has][u8 clsLen][cls][u8 nameLen][name][u8 itemLen][item], then, for the main hand when it
// holds something, the hold transform (relPos, relRot: six floats). The off hand's item sits at the puppet's own
// off-hand component, so it carries no transform.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::hand_item_wire {

// The cap on each string: class names and item names are short ASCII, one length byte each.
inline constexpr size_t kMaxStr = 63;

// Which hand a record describes: mainPlayer_C.holdingActor_main or holdingActor_offhand.
enum class Hand : uint8_t { Main = 0, Off = 1 };

struct Record {
    uint8_t      slot = 0;
    Hand         hand = Hand::Main;
    bool         has = false;
    std::wstring cls;    // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;   // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
    std::wstring item;   // the hand's item name (holdingObjectName_*), the key of the game's item tables
    float        relPos[3] = {0.f, 0.f, 0.f};
    float        relRot[3] = {0.f, 0.f, 0.f};
    bool         haveRel = false;  // the main hand's transform is present and sane
};

// The largest record: slot, hand, has, three length-prefixed strings at the cap, the hold transform.
inline constexpr size_t kMaxRecordBytes = 3 + 3 * (1 + kMaxStr) + 6 * sizeof(float);

// The payload of `r`; each string cut to kMaxStr, the transform written for a held main hand.
std::vector<uint8_t> Build(const Record& r);

// Parse a payload into `out`. Empty on success; else what was wrong with it, for the receiver's log. Each string
// must be within the cap and free of control characters (a NUL would end the name the class index reads before the
// string does, and a line break would split the log lines that quote it). A main-hand transform that is not finite
// or out of arm's reach parses with haveRel false.
std::string Parse(const uint8_t* p, size_t len, Record& out);

}  // namespace coop::hand_item_wire
