// coop/wisp_grab_hold.cpp -- see coop/wisp_grab_hold.h.

#include "coop/creatures/wisp_grab_hold.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/wisp.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::wisp_grab_hold {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Victim-side self grab (game-thread only). eid 0 = not engaged. The retry window
// exists because the WispGrab reliable can beat the wisp mirror's spawn by a frame.
uint32_t g_selfWispEid     = 0;
bool     g_selfAttached    = false;
uint64_t g_selfRetryUntil  = 0;

// Cross-peer puppet holds (host: the real wisp; third peers: the mirror). Tiny set
// (at most one victim per wisp, at most kMaxPeers-1 victims).
struct Hold { uint8_t slot; uint32_t wispEid; };
std::vector<Hold> g_holds;

// Resolve the LOCAL actor for a wisp Element id (the real wisp on the host, the
// npc mirror on a client -- both live in the unified element Registry). Null if
// despawned / not yet spawned / not a killerwisp.
void* ResolveWispActor(uint32_t wispEid) {
    auto* el = coop::element::Registry::Get().Get(
        static_cast<coop::element::ElementId>(wispEid));
    if (!el) return nullptr;
    void* actor = el->GetActor();
    if (!actor || !R::IsLive(actor) || !ue_wrap::wisp::IsKillerWisp(actor)) return nullptr;
    return actor;
}

// One victim-side attach attempt. True once attached (latched by the caller).
bool TryAttachSelf() {
    void* wisp = ResolveWispActor(g_selfWispEid);
    if (!wisp) return false;
    void* local = coop::players::Registry::Get().Local();
    if (!local || !R::IsLive(local)) return false;
    if (!ue_wrap::wisp::ApplyGrabToLocalPlayer(wisp, local)) return false;
    UE_LOGI("wisp_hold[self]: grabbed by wispEid=%u -- riding the mirror's 'playerGrab' "
            "socket until the scheduled death", g_selfWispEid);
    return true;
}

}  // namespace

void EngageSelf(uint32_t wispEid, uint32_t killDelayMs) {
    if (wispEid == 0) return;
    g_selfWispEid    = wispEid;
    g_selfAttached   = false;
    g_selfRetryUntil = NowMs() + killDelayMs;
    if (TryAttachSelf()) g_selfAttached = true;
    // else: Tick retries until the deadline (mirror spawn race) -- the flat death
    // remains the floor if the mirror never resolves.
}

void EngagePuppet(uint32_t wispEid, uint8_t victimSlot) {
    if (wispEid == 0 || victimSlot == coop::players::kPeerIdUnknown) return;
    for (auto& h : g_holds) {
        if (h.slot == victimSlot) { h.wispEid = wispEid; return; }  // idempotent re-engage
    }
    g_holds.push_back(Hold{victimSlot, wispEid});
    UE_LOGI("wisp_hold: holding puppet slot=%u at wispEid=%u 'playerGrab' socket "
            "(releases when the wisp despawns)", victimSlot, wispEid);
}

void ReleaseSelf() {
    if (g_selfWispEid == 0) return;
    if (g_selfAttached) {
        void* local = coop::players::Registry::Get().Local();
        if (local && R::IsLive(local)) ue_wrap::wisp::ReleaseGrabOnLocalPlayer(local);
    }
    g_selfWispEid = 0;
    g_selfAttached = false;
    g_selfRetryUntil = 0;
}

void Tick() {
    // Victim-side attach retry (mirror spawn race), bounded by the kill deadline.
    if (g_selfWispEid != 0 && !g_selfAttached) {
        if (NowMs() > g_selfRetryUntil) {
            UE_LOGW("wisp_hold[self]: wisp mirror eid=%u never resolved inside the grab "
                    "window -- the flat death stands (no ride)", g_selfWispEid);
            g_selfWispEid = 0;
            g_selfRetryUntil = 0;
        } else if (TryAttachSelf()) {
            g_selfAttached = true;
        }
    }
    // Puppet holds: snap each held puppet to its wisp's socket. Runs AFTER the puppet
    // pose apply (net_pump ordering), so this write is the one the frame renders.
    if (g_holds.empty()) return;
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        void* wisp = ResolveWispActor(it->wispEid);
        if (!wisp) {
            UE_LOGI("wisp_hold: wispEid=%u gone -- released puppet slot=%u (pose stream "
                    "resumes)", it->wispEid, it->slot);
            it = g_holds.erase(it);
            continue;
        }
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(it->slot);
        void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
        if (!puppet || !R::IsLive(puppet)) {
            UE_LOGI("wisp_hold: puppet slot=%u gone -- dropping its hold", it->slot);
            it = g_holds.erase(it);
            continue;
        }
        ue_wrap::FVector sock{};
        if (ue_wrap::wisp::GrabSocketWorldLocation(wisp, sock))
            E::SetActorLocation(puppet, sock);
        ++it;
    }
}

void OnPeerLeft(uint8_t slot) {
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        if (it->slot == slot) {
            UE_LOGI("wisp_hold: peer slot=%u left -- dropping its hold", slot);
            it = g_holds.erase(it);
        } else {
            ++it;
        }
    }
}

void OnDisconnect() {
    ReleaseSelf();  // a mid-grab teardown must not strand the local player in MOVE_None
    g_holds.clear();
}

}  // namespace coop::wisp_grab_hold
