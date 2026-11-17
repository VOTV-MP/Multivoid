// coop/props/glowstick_stick_record.h -- the stick fields of a glowstick's save record, read in one place.
//
// The game's own getData lays a glowstick's stick out as: vectors[0] = {loc, relLoc1, relLoc2}, transforms[0][0] =
// the struck component's transform, names[1][0] = attachToKey, bools[1] = {sticked, stickStatic}. The host's
// judge of a client's stick (glowstick_stick_judge), the author's list of its sticks in flight and the landing that
// makes a stick read them here. A record whose
// layout is not that one, or whose floats are not finite, is no honest glowstick's: the read refuses it.
#pragma once

#include "ue_wrap/actors/save_record.h"

#include <string>

namespace coop::glowstick_stick_record {

struct StickFields {
    bool sticked = false;
    bool stickStatic = false;
    float loc[3] = {};
    float rel1[3] = {};
    float rel2[3] = {};
    float xform[10] = {};   // quat xyzw, translation xyz, scale xyz: the layout of a packed record transform
    std::wstring attachKey;  // names[1][0]; empty when the game wrote None
};

enum class Read : uint8_t { Ok, Malformed, NonFinite };

// The record's stick fields. Malformed: a group the layout above names is missing or short. NonFinite: a float of
// the stick is NaN or infinite (a point no physics constraint may be given).
Read ReadStick(const ue_wrap::save_record::SaveRecord& rec, StickFields& out);

// Every stick field equal, floats bit for bit: two records one body carried are equal, a record of another act is not.
bool Same(const StickFields& a, const StickFields& b);

}  // namespace coop::glowstick_stick_record
