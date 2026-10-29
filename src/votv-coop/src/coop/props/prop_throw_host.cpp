// coop/props/prop_throw_host.cpp -- see coop/props/prop_throw.h. The host's side of a client's throw: the bounds
// its release and its drop intent meet, and the release's relay to the other clients.

#include "coop/props/prop_throw.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::prop_throw {
namespace {

namespace tb = coop::net::throw_bounds;

uint32_t g_releaseClamped = 0, g_intentClamped = 0, g_intentRefused = 0, g_relayed = 0;
uint32_t g_relSeen = 0, g_relThrown = 0, g_intSeen = 0, g_intThrown = 0;   // what the host received, for the throw arms

}  // namespace

void JudgeRelease(coop::net::Session& session, int senderSlot, coop::net::PropReleasePayload& p) {
    static const bool s_noClamp = coop::config::ResolveFlag(coop::config_registry::rows::container_throw_no_clamp);
    if (session.role() != coop::net::Role::Host || senderSlot < 1) return;
    ++g_relSeen;
    if (p.throwSeq != 0) ++g_relThrown;
    float lin[3] = {p.linVelX, p.linVelY, p.linVelZ};
    float ang[3] = {p.angVelX, p.angVelY, p.angVelZ};
    // Finite already: the dispatcher refuses a release whose floats are not.
    // Every release meets the body's spin ceiling, thrown or not: throwShit ADDS its spin to the held body's
    // (SetPhysicsAngularVelocityInDegrees bAddToCurrent, mainPlayer.cpp:15615), so a grabbed prop leaves faster than the
    // throw's own 450 deg/s (measured 1905 deg/s on a natural grab throw, cut to 538 by
    // the throw ceiling). The drop intent of a hand throw is a fresh spawn and keeps the throw ceiling.
    if (!s_noClamp && tb::ClampPair(lin, tb::kMaxLaunchSpeedCmS, ang, tb::kMaxBodyAngDegS) == tb::Clamp::Clamped) {
        UE_LOGW("prop_throw: HOST clamped slot %d's release velocity lin=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) "
                "ang=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) (#%u)", senderSlot, p.linVelX, p.linVelY, p.linVelZ, lin[0],
                lin[1], lin[2], p.angVelX, p.angVelY, p.angVelZ, ang[0], ang[1], ang[2], ++g_releaseClamped);
        p.linVelX = lin[0]; p.linVelY = lin[1]; p.linVelZ = lin[2];
        p.angVelX = ang[0]; p.angVelY = ang[1]; p.angVelZ = ang[2];
    }
    // The relay, after the clamp: every world-ready client but the sender, the sender's slot kept as the origin. A
    // slot that is not ready is skipped; its join seed brings the prop's state.
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == senderSlot || !session.IsSlotWorldReady(slot)) continue;
        if (session.SendReliableToSlot(slot, coop::net::ReliableKind::PropRelease, &p, sizeof(p),
                                       static_cast<uint8_t>(senderSlot)))
            ++g_relayed;
    }
}

bool JudgeDropIntent(coop::net::PropDropIntentPayload& p, uint8_t senderSlot) {
    ++g_intSeen;
    if (p.throwSeq != 0) ++g_intThrown;
    static const bool s_noClamp = coop::config::ResolveFlag(coop::config_registry::rows::container_throw_no_clamp);
    if (s_noClamp) return true;   // the clamp arm's red: the forged velocity reaches the spawn
    const float was[3] = {p.linVel[0], p.linVel[1], p.linVel[2]};
    // Only a hand throw's drop intent takes the throw spin ceiling: its item is a fresh spawn (see JudgeRelease).
    const tb::Clamp c = tb::ClampPair(p.linVel, tb::kMaxLaunchSpeedCmS, p.angVel,
                                      p.throwSeq != 0 ? tb::kMaxLaunchAngDegS : tb::kMaxBodyAngDegS);
    if (c == tb::Clamp::NonFinite) {
        UE_LOGW("prop_throw: HOST refused slot %u's drop intent: a velocity is not finite (#%u)", senderSlot,
                ++g_intentRefused);
        return false;
    }
    if (c == tb::Clamp::Clamped)
        UE_LOGW("prop_throw: HOST clamped slot %u's drop intent velocity lin=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) (#%u)",
                senderSlot, was[0], was[1], was[2], p.linVel[0], p.linVel[1], p.linVel[2], ++g_intentClamped);
    return true;
}

namespace dev {
uint32_t ReleasesSeen() { return g_relSeen; }
uint32_t ReleasesThrown() { return g_relThrown; }
uint32_t IntentsSeen() { return g_intSeen; }
uint32_t IntentsThrown() { return g_intThrown; }
}  // namespace dev

}  // namespace coop::prop_throw
