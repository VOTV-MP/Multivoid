// coop/held_clump_sync.cpp -- see coop/held_clump_sync.h.
//
// MTA attach model for the non-keyable trash clump. Per-holder-slot the receiver
// keeps ONE attached mirror; on release it detaches + free-falls into a bounded
// ring (cleanup, since the clump has no key/eid the existing PropDestroy could
// carry). All state is game-thread only (sender = net_pump tick; receiver =
// event_feed GT::Post; cleanup = net_pump disconnect edge) -- no mutex.

#include "coop/held_clump_sync.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::held_clump_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// One attached mirror per holder slot (you can only hold one clump at a time).
struct Mirror {
    void*   actor = nullptr;
    int32_t idx   = -1;     // GUObjectArray InternalIndex for IsLiveByIndex
};
Mirror g_held[coop::players::kMaxPeers];

// Released (free-falling/settled) mirrors. The clump has no cross-peer key/eid the
// existing PropDestroy path could carry, so we can't despawn-sync them exactly --
// this fixed FIFO ring caps accumulation (oldest destroyed when full). Generous so
// normal carry-then-drop never visibly evicts; documented follow-up = real
// despawn-sync. [[project-bug-trash-chippile-uaf-crash]]
constexpr int kReleasedCap = 24;
Mirror g_released[kReleasedCap];
int    g_releasedNext = 0;

bool Live(const Mirror& m) { return m.actor && R::IsLiveByIndex(m.actor, m.idx); }

void DestroyIfLive(Mirror& m) {
    if (Live(m)) E::DestroyActor(m.actor);
    m.actor = nullptr;
    m.idx = -1;
}

// Hand a still-living mirror to the FIFO ring (it keeps free-falling). Evicts +
// destroys whatever occupied the write slot.
void PushReleased(const Mirror& m) {
    if (!Live(m)) return;
    DestroyIfLive(g_released[g_releasedNext]);
    g_released[g_releasedNext] = m;
    g_releasedNext = (g_releasedNext + 1) % kReleasedCap;
}

uint32_t LocalEidOrZero() {
    const coop::element::ElementId eid =
        coop::players::Registry::Get().LocalPlayerElementId();
    return (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
}

std::wstring ClassNameFromWire(const coop::net::WireClassName& w) {
    std::wstring s;
    const int n = (w.len <= 63) ? w.len : 63;
    for (int i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(w.data[i])));
    return s;
}

}  // namespace

bool IsAttachClump(void* heldActor) {
    if (!heldActor) return false;
    // A trash clump = a keyed interactable that is NOT an Aprop_C. The Aprop_C
    // lineage rides the keyed prop pipeline (trash_collect_sync + PropPose); the
    // non-Aprop_C keyed bases (chipPile / garbageClump / trashBitsPile) are the
    // transient, non-keyable trash this attach model exists for.
    return ue_wrap::prop::IsKeyedInteractable(heldActor) &&
           !ue_wrap::prop::IsDescendantOfProp(heldActor);
}

bool SendGrab(void* clumpActor, coop::net::Session* s) {
    if (!clumpActor || !s || !s->connected() || !R::IsLive(clumpActor)) return false;
    const std::wstring cls = R::ClassNameOf(clumpActor);
    coop::net::HeldClumpGrabPayload p{};
    p.senderElementId = LocalEidOrZero();
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    const bool sent = s->SendReliable(coop::net::ReliableKind::HeldClumpGrab, &p, sizeof(p));
    UE_LOGI("held_clump: SEND grab cls='%ls' senderEid=0x%08x sent=%d",
            cls.c_str(), p.senderElementId, sent ? 1 : 0);
    return sent;
}

void SendRelease(void* clumpActor, coop::net::Session* s) {
    if (!s || !s->connected()) return;
    coop::net::HeldClumpReleasePayload p{};
    p.senderElementId = LocalEidOrZero();
    // Capture the clump's throw velocity ONCE off the LIVE actor (generic root
    // primitive -- never the Aprop_C mesh offset, so no UAF on the morphing clump).
    if (clumpActor && R::IsLive(clumpActor)) {
        ue_wrap::FVector lin{}, ang{};
        if (E::GetActorRootPhysicsVelocity(clumpActor, lin, ang)) {
            p.linVelX = lin.X; p.linVelY = lin.Y; p.linVelZ = lin.Z;
            p.angVelX = ang.X; p.angVelY = ang.Y; p.angVelZ = ang.Z;
        }
    }
    const float linMag = std::sqrt(p.linVelX * p.linVelX + p.linVelY * p.linVelY + p.linVelZ * p.linVelZ);
    s->SendReliable(coop::net::ReliableKind::HeldClumpRelease, &p, sizeof(p));
    UE_LOGI("held_clump: SEND release senderEid=0x%08x |linVel|=%.1f cm/s",
            p.senderElementId, linMag);
}

void ApplyGrab(uint8_t peerSlot, void* puppetActor, const coop::net::HeldClumpGrabPayload& p) {
    if (peerSlot >= coop::players::kMaxPeers) return;
    if (!puppetActor || !R::IsLive(puppetActor)) {
        UE_LOGW("held_clump: ApplyGrab slot %u has no live puppet -- dropping grab "
                "(next grab mirrors once the puppet exists)", static_cast<unsigned>(peerSlot));
        return;
    }
    const std::wstring cls = ClassNameFromWire(p.className);
    if (cls.empty()) { UE_LOGW("held_clump: ApplyGrab empty class name -- dropping"); return; }
    void* clumpCls = R::FindClass(cls.c_str());
    if (!clumpCls) { UE_LOGW("held_clump: ApplyGrab class '%ls' not found -- dropping", cls.c_str()); return; }

    // Replace any prior held mirror for this slot (stale -- no release was seen).
    // Treat it like a release so it free-falls into the ring instead of leaking
    // attached to a hand that's about to hold a different clump.
    if (Live(g_held[peerSlot])) {
        E::DetachActorFromParent(g_held[peerSlot].actor);
        E::SetActorSimulatePhysics(g_held[peerSlot].actor, true);
        PushReleased(g_held[peerSlot]);
    }
    g_held[peerSlot] = {};

    // Spawn the mirror at the puppet pivot; the attach snaps it to the hand bone.
    const ue_wrap::FVector at = E::GetActorLocation(puppetActor);
    void* mirror = E::SpawnActor(clumpCls, at, /*inertPawn=*/false);
    if (!mirror || !R::IsLive(mirror)) {
        UE_LOGW("held_clump: ApplyGrab SpawnActor('%ls') failed -- no mirror", cls.c_str());
        return;
    }
    // Capture the GUObjectArray index IMMEDIATELY after spawn, while the mirror is
    // provably live (the established prop_element_tracker/remote_prop_spawn pattern),
    // BEFORE any other engine call. If it can't be captured (-1) the mirror can't be
    // safely tracked via IsLiveByIndex (Live() would call IsLiveByIndex(mirror,-1),
    // undefined) -- discard it. Audit finding (2026-06-02).
    const int32_t mirrorIdx = R::InternalIndexOf(mirror);
    if (mirrorIdx < 0) {
        E::DestroyActor(mirror);
        UE_LOGW("held_clump: ApplyGrab InternalIndexOf failed on fresh mirror '%ls' -- discarding",
                cls.c_str());
        return;
    }
    // Kinematic while held (a simulating root would ignore the attach), then attach
    // to the hand. The mirror is OUR actor -- never grabbed, so it never morphs/
    // self-frees on interaction the way the holder's real clump does.
    E::SetActorSimulatePhysics(mirror, false);
    const bool attached = E::AttachActorToPuppetHand(mirror, puppetActor);
    g_held[peerSlot].actor = mirror;
    g_held[peerSlot].idx   = mirrorIdx;
    UE_LOGI("held_clump: ApplyGrab slot %u spawned+%s mirror '%ls' @%p (idx=%d)",
            static_cast<unsigned>(peerSlot), attached ? "attached" : "ATTACH-FAILED",
            cls.c_str(), mirror, g_held[peerSlot].idx);
}

void ApplyRelease(uint8_t peerSlot, const coop::net::HeldClumpReleasePayload& p) {
    if (peerSlot >= coop::players::kMaxPeers) return;
    if (!Live(g_held[peerSlot])) {
        // Nothing held (the grab was dropped for a missing puppet, or already
        // released) -- just clear the slot.
        g_held[peerSlot] = {};
        return;
    }
    void* m = g_held[peerSlot].actor;
    E::DetachActorFromParent(m);
    E::SetActorSimulatePhysics(m, true);                   // free-fall
    const ue_wrap::FVector lin{ p.linVelX, p.linVelY, p.linVelZ };
    const ue_wrap::FVector ang{ p.angVelX, p.angVelY, p.angVelZ };
    E::SetActorRootPhysicsVelocity(m, lin, ang);           // inherit the throw
    PushReleased(g_held[peerSlot]);                        // hand to the bounded ring
    g_held[peerSlot] = {};
    const float linMag = std::sqrt(p.linVelX * p.linVelX + p.linVelY * p.linVelY + p.linVelZ * p.linVelZ);
    UE_LOGI("held_clump: ApplyRelease slot %u detached+thrown |linVel|=%.1f cm/s",
            static_cast<unsigned>(peerSlot), linMag);
}

void OnDisconnectForSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    DestroyIfLive(g_held[peerSlot]);
}

void OnDisconnect() {
    for (int i = 0; i < static_cast<int>(coop::players::kMaxPeers); ++i)
        DestroyIfLive(g_held[i]);
    for (int i = 0; i < kReleasedCap; ++i)
        DestroyIfLive(g_released[i]);
    g_releasedNext = 0;
}

}  // namespace coop::held_clump_sync
