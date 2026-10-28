// coop/dev/pile_defer_drill_host.cpp -- see coop/dev/pile_defer_drill.h. The trash reader both roles use and the
// host's judge of one joiner's report.

#include "pile_defer_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::dev::pile_defer_drill::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace EL = coop::element;

constexpr int kMaxLines = 20;   // FAIL and moving lines each; the rest is counted in the verdict

float Len(const ue_wrap::FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

// The angle between two rotations' components, degrees, wrapped to [0, 180].
float AngleOff(float a, float b) {
    float d = std::fabs(std::fmod(a - b, 360.f));
    return d > 180.f ? 360.f - d : d;
}

float TurnOff(const coop::net::DevParityEntry& a, const coop::net::DevParityEntry& b) {
    return std::max({AngleOff(a.pitch, b.pitch), AngleOff(a.yaw, b.yaw), AngleOff(a.roll, b.roll)});
}

float PlaceOff(const coop::net::DevParityEntry& a, const coop::net::DevParityEntry& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The host's speed of the pile of `eid`, or -1 when it has no live actor to read.
float HostSpeed(uint32_t eid) {
    void* a = EL::LivePropActor(static_cast<EL::ElementId>(eid));
    return a ? Len(ue_wrap::engine::GetActorVelocity(a)) : -1.f;
}

}  // namespace

void CollectTrash(std::vector<coop::net::DevParityEntry>& out, bool mirrorsOnly) {
    out.clear();
    std::vector<EL::Registry::ActorIdPair> pairs;
    EL::Registry::Get().SnapshotActorsByType(EL::ElementType::Prop, pairs);
    for (const auto& pr : pairs) {
        if (!pr.actor || !EL::Registry::IsAllowedHostAllocatedEid(pr.id)) continue;
        if (mirrorsOnly && !pr.mirror) continue;
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ue_wrap::prop::IsTrashActor(pr.actor)) continue;
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!ue_wrap::engine::TryGetActorLocation(pr.actor, at) ||
            !ue_wrap::engine::TryGetActorRotation(pr.actor, rot))
            continue;   // no read, no row: the host then lists the eid as missing, which is true of the report
        coop::net::DevParityEntry e{};
        e.eid = static_cast<uint32_t>(pr.id);
        e.form = ue_wrap::prop::IsGarbageClump(pr.actor) ? 1 : 0;
        e.x = at.X; e.y = at.Y; e.z = at.Z;
        e.pitch = rot.Pitch; e.yaw = rot.Yaw; e.roll = rot.Roll;
        out.push_back(e);
    }
}

void JudgeReport(const coop::dev::pile_defer_report::Report& report) {
    std::vector<coop::net::DevParityEntry> mine;
    CollectTrash(mine, /*mirrorsOnly=*/false);
    std::unordered_map<uint32_t, const coop::net::DevParityEntry*> hostBy;
    hostBy.reserve(mine.size());
    for (const auto& e : mine) hostBy[e.eid] = &e;

    unsigned compared = 0, missing = 0, moving = 0, extra = 0, failed = 0, failLines = 0, movingLines = 0;
    float worstCm = 0.f, worstDeg = 0.f;
    auto fail = [&](const char* fmt, auto... args) {
        ++failed;
        if (failLines++ < kMaxLines) UE_LOGW(fmt, args...);
    };
    auto listMoving = [&](uint32_t eid, float speed, const char* why) {
        ++moving;
        if (movingLines++ < kMaxLines)
            UE_LOGI("[PILE-DEFER] host moving eid=%u (%s, %.1f cm/s) listed, not judged", eid, why, speed);
    };

    std::unordered_map<uint32_t, bool> reported;
    reported.reserve(report.rows.size());
    for (const auto& j : report.rows) {
        if (!reported.emplace(j.eid, true).second) {
            fail("[PILE-DEFER] host FAIL: eid=%u reported twice by the joiner", j.eid);
            continue;
        }
        const auto it = hostBy.find(j.eid);
        if (it == hostBy.end()) { ++extra; continue; }   // the host has no such pile now: said in the verdict
        const coop::net::DevParityEntry& h = *it->second;
        const float speed = HostSpeed(j.eid);
        if (h.form == 1) { listMoving(j.eid, speed, "a clump"); continue; }
        if (speed > kMovingCmS) { listMoving(j.eid, speed, "a pile in motion"); continue; }
        ++compared;
        if (j.form != h.form) {
            fail("[PILE-DEFER] host FAIL: eid=%u form off by host=%u joiner=%u", j.eid, h.form, j.form);
            continue;
        }
        const float cm = PlaceOff(h, j), deg = TurnOff(h, j);
        worstCm = std::max(worstCm, cm);
        worstDeg = std::max(worstDeg, deg);
        if (cm > kPlaceCm) fail("[PILE-DEFER] host FAIL: eid=%u place off by %.1f cm", j.eid, cm);
        if (deg > kTurnDeg) fail("[PILE-DEFER] host FAIL: eid=%u turn off by %.1f deg", j.eid, deg);
    }
    for (const auto& h : mine) {
        if (reported.count(h.eid)) continue;
        const float speed = HostSpeed(h.eid);
        if (h.form == 1) { listMoving(h.eid, speed, "a clump the joiner lacks"); continue; }
        if (speed > kMovingCmS) { listMoving(h.eid, speed, "a pile in motion the joiner lacks"); continue; }
        ++missing;
        fail("[PILE-DEFER] host FAIL: missing eid=%u", h.eid);
    }
    if (failLines > kMaxLines) UE_LOGW("[PILE-DEFER] host FAIL: %u more line(s) not printed", failLines - kMaxLines);

    if (failed == 0)
        UE_LOGI("[PILE-DEFER] host PASS: n=%u compared, missing=%u, worst %.1f cm / %.1f deg "
                "(slot %u, %zu reported, %u moving listed, %u reported with no host pile)",
                compared, missing, worstCm, worstDeg, static_cast<unsigned>(report.slot), report.rows.size(), moving,
                extra);
    UE_LOGI("[PILE-DEFER] host DONE");
}

}  // namespace coop::dev::pile_defer_drill::detail
