// coop/props/prop_throw_host.cpp -- see coop/props/prop_throw.h. The host's side of a client's throw: the bounds
// its release and its drop intent meet.

#include "coop/props/prop_throw.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::prop_throw {
namespace {

namespace tb = coop::net::throw_bounds;

uint32_t g_releaseClamped = 0;

}  // namespace

void JudgeRelease(coop::net::Session& session, int senderSlot, coop::net::PropReleasePayload& p) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1) return;
    float lin[3] = {p.linVelX, p.linVelY, p.linVelZ};
    float ang[3] = {p.angVelX, p.angVelY, p.angVelZ};
    // Finite already: the dispatcher refuses a release whose floats are not.
    if (tb::ClampPair(lin, tb::kMaxLaunchSpeedCmS, ang, tb::kMaxLaunchAngDegS) != tb::Clamp::Clamped) return;
    UE_LOGW("prop_throw: HOST clamped slot %d's release velocity lin=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) "
            "ang=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) (#%u)", senderSlot, p.linVelX, p.linVelY, p.linVelZ, lin[0],
            lin[1], lin[2], p.angVelX, p.angVelY, p.angVelZ, ang[0], ang[1], ang[2], ++g_releaseClamped);
    p.linVelX = lin[0]; p.linVelY = lin[1]; p.linVelZ = lin[2];
    p.angVelX = ang[0]; p.angVelY = ang[1]; p.angVelZ = ang[2];
}

}  // namespace coop::prop_throw
