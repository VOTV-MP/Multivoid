// coop/wisp_tear_mirror.cpp -- see coop/wisp_tear_mirror.h.

#include "coop/social/wisp_tear_mirror.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/players_registry.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/wisp.h"

#include <chrono>
#include <cstdint>

namespace coop::wisp_tear_mirror {
namespace {

namespace R = ue_wrap::reflection;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Armed victim ragdoll-death deadline (0 = not armed). Game-thread only (set by OnWispGrab,
// discharged by Tick -- both on the game thread, so a plain value is safe).
uint64_t g_killAtMs = 0;
uint32_t g_killWispEid = 0;  // for logging only

}  // namespace

void OnWispGrab(const coop::net::WispGrabPayload& p, uint8_t senderPeerSlot) {
    // Host-only origin (the host detected its wisp false-grabbing OUR puppet). Belt-and-
    // suspenders self-verify: the host directs this to our slot, but also confirm the
    // addressed Player Element id is OURS before we schedule our own death.
    if (senderPeerSlot != 0) {
        UE_LOGW("wisp_tear: WispGrab from non-host slot=%u -- dropping", senderPeerSlot);
        return;
    }
    const coop::element::ElementId localEid = coop::players::Registry::Get().LocalPlayerElementId();
    if (p.victimElementId != static_cast<uint32_t>(localEid)) {
        UE_LOGW("wisp_tear: WispGrab victimElementId=%u != our localPlayerElementId=%u -- dropping "
                "(misaddressed)", p.victimElementId, static_cast<uint32_t>(localEid));
        return;
    }
    // Sanitize the host-decided delay (clamp to a sane window so a corrupt packet can't park
    // the death forever or fire it instantly).
    uint32_t delay = p.killDelayMs;
    if (delay < 250u)  delay = 250u;
    if (delay > 8000u) delay = 8000u;
    g_killAtMs = NowMs() + delay;
    g_killWispEid = p.wispElementId;
    UE_LOGI("wisp_tear[victim]: WispGrab accepted (wispEid=%u) -- scheduling OUR ragdoll death in %u ms",
            p.wispElementId, delay);
}

void PlayTearOnWisp(void* wispActor, uint32_t victimSlot) {
    if (!wispActor || !R::IsLive(wispActor) || !ue_wrap::wisp::IsKillerWisp(wispActor)) return;
    // Force the parked mirror mesh to tick so a played montage advances, then play the
    // fatality tear. Both best-effort: the force-tick alone shows the mirror; the montage is
    // the gore. (Gibs + the victim socket-attach hold are a documented follow-on.)
    ue_wrap::wisp::ForceMeshTick(wispActor);
    const bool montage = ue_wrap::wisp::PlayFatalityMontage(wispActor);
    UE_LOGI("wisp_tear: tear on wisp actor=%p (victimSlot=%u) -- force-tick + montage=%d",
            wispActor, victimSlot, montage ? 1 : 0);
}

void OnWispTear(const coop::net::WispTearPayload& p, uint8_t senderPeerSlot) {
    if (senderPeerSlot != 0) {
        UE_LOGW("wisp_tear: WispTear from non-host slot=%u -- dropping", senderPeerSlot);
        return;
    }
    // Resolve the LOCAL wisp actor by its cross-peer Element id (a client mirror in
    // NpcMirrors, registered into the unified element Registry). null if the wisp already
    // despawned (race) -- then the tear is a no-op, which is fine.
    auto* el = coop::element::Registry::Get().Get(
        static_cast<coop::element::ElementId>(p.wispElementId));
    if (!el) {
        UE_LOGI("wisp_tear: WispTear wispEid=%u not resolvable locally (mirror not present / despawned)",
                p.wispElementId);
        return;
    }
    PlayTearOnWisp(el->GetActor(), p.victimSlot);
}

void Tick() {
    if (g_killAtMs == 0) return;
    if (NowMs() < g_killAtMs) return;
    g_killAtMs = 0;
    void* local = coop::players::Registry::Get().Local();
    if (!local || !R::IsLive(local)) {
        UE_LOGW("wisp_tear[victim]: kill deadline elapsed but local player not live -- skipping");
        return;
    }
    // The kill = DIRECT ragdollMode(true,false,true) (NOT kill() -- see the RE addendum:
    // kill() no-ops behind immortal/startInvinc, ragdollMode does not). Native death -> menu
    // -> rejoin (the settled permadeath-rejoinable policy).
    const bool ok = ue_wrap::engine::SetMainPlayerRagdollMode(local, /*ragdoll=*/true,
                                                              /*passOut=*/false, /*death=*/true);
    UE_LOGI("wisp_tear[victim]: ragdoll DEATH fired (wispEid=%u) ok=%d -- native death->menu->rejoin",
            g_killWispEid, ok ? 1 : 0);
    g_killWispEid = 0;
}

void OnDisconnect() {
    g_killAtMs = 0;
    g_killWispEid = 0;
}

}  // namespace coop::wisp_tear_mirror
