// coop/dev/swinger_drill_pose.h -- [dev] where a lid's parent prop is, for the swinger drill's lines. A lid that
// hangs on a prop (a barrel's cap) is joined to it by a constraint, so the pose of the lid in the world follows the
// parent's; the drill prints that parent beside the lid's own pose, and the client carries its parent's pose in its
// `settled` chat line so the host can set the two beside each other. See swinger_drill.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::swinger_drill_pose {

// The actor a lid is attached to, with its place and orientation. `name` empty: the lid has no parent;
// `has` false with a name: the parent's pose could not be read.
struct ParentPose {
    std::string name;
    bool has = false;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
};

// The parent of the lid actor `door` (null reads as none). Game thread.
ParentPose ReadParent(void* door);

// "parent '<name>' loc (x, y, z) rot (p, y, r)", or "parent none" / "parent '<name>' unread".
std::string Describe(const ParentPose& parent);

// The client's chat line when its door settled: "settled <p> <y> <r> <x> <y> <z>" for the door's rotation and
// place, then the parent's "<p> <y> <r> <x> <y> <z>", or "none" with no readable parent.
std::string SettledLine(const ue_wrap::FRotator& rot, const ue_wrap::FVector& at, const ParentPose& parent);

// The words of a `settled` line: false when it is not one or is malformed. The parent's name is not on the line.
bool ParseSettled(const std::vector<std::string>& word, ue_wrap::FRotator* rot, ue_wrap::FVector* at,
                  ParentPose* parent);

// The host's verdict tail: the client's parent pose beside this peer's, and how far apart the two are.
std::string VerdictParent(const ParentPose& mine, const ParentPose& client);

// The parent test of the host's verdict: both parents read and within the settle tolerance, or neither lid has one.
// False names the reason in `why` (one side unread, or the two apart).
bool ParentsAgree(const ParentPose& mine, const ParentPose& client, std::string* why);

// The mechanism test of the host's verdict: a parent of `door` that simulates physics on this (host) peer must have
// had at least kParentRows rows admitted from its syncer, the dragging client. A lid with no parent, or a parent that
// does not simulate, needs none. `rows` is the count read (0 when none was); `why` names the finding.
inline constexpr uint32_t kParentRows = 3;
bool ParentSynced(void* door, uint32_t* rows, std::string* why);

}  // namespace coop::dev::swinger_drill_pose
