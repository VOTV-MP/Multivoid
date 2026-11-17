// coop/dev/rest_compare.h -- TEST-ONLY: how far apart two peers' copies of one prop came to rest.
//
// The prop-rest drills (body push, coast let-go, extinguisher runaway) judge one thing: the host's copy and a client's
// copy of a prop, each at its own rest, stand within 1 cm and 1 degree of each other. The client says its rest pose in a
// chat line (coop/dev/drill_chat) and the host, which judges, reads its own and compares: the verdict is decided the
// moment both have marked their rest, and a gap past the bound is a FAIL said at once. A time bound elsewhere may say
// INVALID and nothing else. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace coop::dev::rest_compare {

constexpr float kBoundCm  = 1.f;
constexpr float kBoundDeg = 1.f;

struct Pose {
    ue_wrap::FVector  loc{};
    ue_wrap::FRotator rot{};
};

// `actor`'s place and rotation; false when either cannot be read.
bool Read(void* actor, Pose& out);

// The pose as the text of a chat line, and back; Decode false when `text` is not an encoded pose.
std::string Encode(const Pose& p);
bool Decode(const std::string& text, Pose& out);

// The distance between two places, and the angle of the one turn that takes the first rotation to the second: two equal
// orientations read 0 whatever Euler triple names them (near a pitch of 90 degrees the triples differ by a lot).
float Cm(const Pose& a, const Pose& b);
float Deg(const Pose& a, const Pose& b);

// Judges `host` against `client` for the line `what`: logs both numbers, and past either bound says
// `rig: VERDICT <tag> FAIL` naming `what` and returns false. Within both, returns true and says nothing. With `sayFail`
// false a gap past the bound is only logged and returned: a red arm built to fail a later leg says INVALID for an earlier
// one, so its FAIL is the lever's own.
bool Judge(const char* tag, const char* what, const Pose& host, const Pose& client, bool sayFail = true);

}  // namespace coop::dev::rest_compare
