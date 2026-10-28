// coop/dev/swinger_drill_pose.cpp -- see coop/dev/swinger_drill_pose.h.

#include "coop/dev/swinger_drill_pose.h"

#include "coop/dev/swinger_drill_join.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_syncer_host.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cstdio>
#include <cstdlib>

namespace coop::dev::swinger_drill_pose {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

}  // namespace

ParentPose ReadParent(void* door) {
    ParentPose p;
    void* const parent = door ? E::ParentActorOf(door) : nullptr;
    if (!parent || !R::IsLive(parent)) return p;
    const std::wstring name = R::ToString(R::NameOf(parent));
    p.name.assign(name.begin(), name.end());
    if (p.name.empty()) p.name = "?";
    p.has = E::TryGetActorLocation(parent, p.loc) && E::TryGetActorRotation(parent, p.rot);
    return p;
}

std::string Describe(const ParentPose& parent) {
    if (parent.name.empty()) return "parent none";
    char buf[192];
    if (!parent.has) {
        snprintf(buf, sizeof(buf), "parent '%s' unread", parent.name.c_str());
    } else {
        snprintf(buf, sizeof(buf), "parent '%s' loc (%.1f, %.1f, %.1f) rot (%.2f, %.2f, %.2f)", parent.name.c_str(),
                 parent.loc.X, parent.loc.Y, parent.loc.Z, parent.rot.Pitch, parent.rot.Yaw, parent.rot.Roll);
    }
    return buf;
}

std::string SettledLine(const ue_wrap::FRotator& rot, const ue_wrap::FVector& at, const ParentPose& parent) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "settled %.2f %.2f %.2f %.1f %.1f %.1f", rot.Pitch, rot.Yaw, rot.Roll, at.X,
                     at.Y, at.Z);
    if (parent.has) {
        snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %.2f %.2f %.2f %.1f %.1f %.1f", parent.rot.Pitch,
                 parent.rot.Yaw, parent.rot.Roll, parent.loc.X, parent.loc.Y, parent.loc.Z);
    } else {
        snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " none");
    }
    return buf;
}

bool ParseSettled(const std::vector<std::string>& word, ue_wrap::FRotator* rot, ue_wrap::FVector* at,
                  ParentPose* parent) {
    if (word.empty() || word[0] != "settled") return false;
    const bool hasParent = word.size() == 13;
    if (!hasParent && !(word.size() == 8 && word[7] == "none")) return false;
    float v[12] = {};
    for (size_t i = 0; i < (hasParent ? 12u : 6u); ++i) v[i] = std::strtof(word[i + 1].c_str(), nullptr);
    *rot = {v[0], v[1], v[2]};
    *at = {v[3], v[4], v[5]};
    *parent = ParentPose{};
    if (hasParent) {
        parent->name = "?";
        parent->has = true;
        parent->rot = {v[6], v[7], v[8]};
        parent->loc = {v[9], v[10], v[11]};
    }
    return true;
}

std::string VerdictParent(const ParentPose& mine, const ParentPose& client) {
    char buf[256];
    if (!mine.has || !client.has) {
        snprintf(buf, sizeof(buf), "client's parent not comparable (this peer's %s, the client's %s)",
                 mine.has ? "read" : "none or unread", client.has ? "read" : "none or unread");
    } else {
        snprintf(buf, sizeof(buf),
                 "client's parent loc (%.1f, %.1f, %.1f) rot (%.2f, %.2f, %.2f): %.1f cm and %.2f deg from this peer's",
                 client.loc.X, client.loc.Y, client.loc.Z, client.rot.Pitch, client.rot.Yaw, client.rot.Roll,
                 swinger_drill_join::Dist(mine.loc, client.loc), E::RotatorAngleDeg(mine.rot, client.rot));
    }
    return buf;
}

bool ParentsAgree(const ParentPose& mine, const ParentPose& client, std::string* why) {
    if (mine.has && client.has) {
        const float cm = swinger_drill_join::Dist(mine.loc, client.loc);
        const float deg = E::RotatorAngleDeg(mine.rot, client.rot);
        if (cm <= swinger_drill_join::kSettleCm && deg <= swinger_drill_join::kSettleDeg) {
            *why = "within tolerance";
            return true;
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "the parents are %.1f cm and %.2f deg apart", cm, deg);
        *why = buf;
        return false;
    }
    if (!mine.has && mine.name.empty() && !client.has) { *why = "no parent on either side"; return true; }
    *why = std::string("one side unread (this peer's ") + (mine.has ? "read" : "none or unread") + ", the client's " +
           (client.has ? "read" : "none or unread") + ")";
    return false;
}

bool ParentSynced(void* door, uint32_t* rows, std::string* why) {
    if (rows) *rows = 0;
    void* const parent = door ? E::ParentActorOf(door) : nullptr;
    if (!parent || !R::IsLive(parent)) { *why = "no parent"; return true; }
    if (!E::IsActorRootSimulatingPhysics(parent)) { *why = "the parent does not simulate"; return true; }
    const uint32_t eid = coop::prop_element_tracker::GetPropElementIdForActor(parent);
    const uint32_t seen = coop::prop_syncer_host::dev::AdmittedOf(eid).rows;
    if (rows) *rows = seen;
    char buf[112];
    snprintf(buf, sizeof(buf), "the parent (eid %u) had %u rows admitted from its syncer, need >= %u", eid, seen,
             kParentRows);
    *why = buf;
    return seen >= kParentRows;
}

}  // namespace coop::dev::swinger_drill_pose
