// coop/dev/basedmove_drill_sides.cpp -- which side of the cargo lift's platform is the way on, for the based-movement drill; see
// coop/dev/basedmove_drill.h. The lift's platform carries rails and stands in a room with walls, and the NavMesh knows neither
// the rails nor which of its sides they close, so the way on is found by traces: the floor in front of a side must be the floor
// the platform rests on, and nothing may stand between that floor point and the slab's centre.

#include "basedmove_drill_detail.h"

#include "ue_wrap/core/trace.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::basedmove_drill::detail {

namespace {

constexpr float kStandOutCm = 70.f;      // the stand point lies this far outside the slab's edge
constexpr float kFloorTolCm = 20.f;      // its floor is within this of the floor under the platform
constexpr float kBodyHalfWidthCm = 20.f; // the capsule's radius: the lines run this far to either side of the line to the centre
constexpr float kHeights[] = {20.f, 60.f, 100.f};   // over the slab's surface: a lip, a rail's waist, a rail's top
constexpr uint8_t kTypes[] = {0, 1, 3};   // WorldStatic, WorldDynamic, PhysicsBody (trace.h)

}  // namespace

std::vector<Side> ProbeSides(void* ctx, const PlatformBox& box, float halfHeight) {
    struct Dir {
        const char* name;
        float dx, dy;
    };
    const Dir dirs[] = {{"+X", 1.f, 0.f}, {"-X", -1.f, 0.f}, {"+Y", 0.f, 1.f}, {"-Y", 0.f, -1.f}};
    std::vector<Side> out;
    for (const Dir& d : dirs) {
        Side s;
        s.name = d.name;
        const float reach = (d.dx != 0.f ? box.extent.X : box.extent.Y) + kStandOutCm;
        const float px = box.centre.X + d.dx * reach, py = box.centre.Y + d.dy * reach;
        s.stand = {px, py, box.floorZ + halfHeight};
        char buf[200];
        ue_wrap::trace::Hit hit;
        // The floor in front of the side: a down-trace there must strike the floor the platform rests on.
        if (!ue_wrap::trace::LineHitForObjects(ctx, {px, py, box.floorZ + 120.f}, {px, py, box.floorZ - 120.f}, kTypes, 3, nullptr, 0,
                                               &hit)) {
            s.why = "the floor trace did not resolve";
        } else if (!hit.blocked || hit.normal.Z < 0.7f || std::fabs(hit.point.Z - box.floorZ) > kFloorTolCm) {
            std::snprintf(buf, sizeof(buf), "no floor level with the platform's (%.1f) there: %s", box.floorZ,
                          hit.blocked ? "the trace struck another height" : "the trace struck nothing");
            s.why = buf;
            if (hit.blocked) {
                std::snprintf(buf, sizeof(buf), " (z=%.1f nz=%.2f)", hit.point.Z, hit.normal.Z);
                s.why += buf;
            }
        } else {
            // The way in: three heights, three lines across the body's width, each from the stand point to the centre.
            const float px2 = -d.dy, py2 = d.dx;   // across the line
            int blocked = 0;
            std::string first;
            for (const float h : kHeights) {
                for (const float lat : {-kBodyHalfWidthCm, 0.f, kBodyHalfWidthCm}) {
                    const float z = box.surface + h;
                    const ue_wrap::FVector a{px + px2 * lat, py + py2 * lat, z};
                    const ue_wrap::FVector b{box.centre.X + px2 * lat, box.centre.Y + py2 * lat, z};
                    ue_wrap::trace::Hit line;
                    if (!ue_wrap::trace::LineHitForObjects(ctx, a, b, kTypes, 3, nullptr, 0, &line) || line.blocked) {
                        if (++blocked == 1) {
                            std::snprintf(buf, sizeof(buf), "height +%.0f, %+.0f across, struck at (%.0f,%.0f,%.0f)", h, lat,
                                          line.point.X, line.point.Y, line.point.Z);
                            first = buf;
                        }
                    }
                }
            }
            s.open = blocked == 0;
            if (s.open) s.why = "floor level with the platform's and nine lines to its centre clear";
            else {
                std::snprintf(buf, sizeof(buf), "%d of 9 lines to the centre struck something; the first: ", blocked);
                s.why = buf + first;
            }
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace coop::dev::basedmove_drill::detail
