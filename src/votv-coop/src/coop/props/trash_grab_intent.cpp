// coop/props/trash_grab_intent.cpp -- the CLIENT-initiated grab/throw intent
// lane of coop::trash_channel (see coop/props/trash_channel.h, "CLIENT-GRAB
// direction" + "CLIENT carry-state").
//
// Extracted from trash_channel.cpp 2026-07-10 when it passed the 800-LOC soft
// cap. Behavior preserved byte-for-byte; the one mechanical change is that the
// core's carry-latch read at GATE 1 goes through the public IsCarrying()
// instead of touching g_carry directly (same find != end).
//
// This TU owns the intent lane's whole state: the HELD_BY registry (eid ->
// holder peer slot -- the door holdOpen_ analog; the core reaches it via the
// three ops in trash_channel_detail.h) and the client-side pending-grab /
// carry toggles. The core (ctx generations, carry latch, land settle, birth
// certificates, TickCarry) stays in trash_channel.cpp.

#include "coop/props/trash_channel.h"

#include "trash_channel_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/registry.h"    // Registry::Get(eid) -- resolve the pile to grab (Increment 2)
#include "coop/player/players_registry.h"    // Puppet(slot) -- the client-grab direction (Increment 2)
#include "coop/player/puppet_carry_drive.h"  // NotePuppetHeld -- host drives the puppet-held clump pose
#include "coop/player/remote_player.h"       // RemotePlayer::GetActor / valid
#include "coop/props/prop_snapshot.h"  // ExpressIncrementalSpawn (wrong-class deny -> re-assert the row)
#include "ue_wrap/core/call.h"        // ParamFrame / Call (the probe-proven puppet-grab pattern)
#include "ue_wrap/engine/engine.h"      // grab-state read + physics/velocity drives
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"        // IsChipPile / IsGarbageClump / GetChipType
#include "ue_wrap/core/reflection.h"  // ClassNameOf / ClassOf / FindFunction
#include "ue_wrap/core/types.h"       // FVector / FRotator

#include <chrono>     // wrong-class deny heal debounce (per-eid, 5 s)
#include <cmath>      // std::sqrt -- clamp the inherited throw velocity (L4)
#include <cstdint>
#include <unordered_map>

namespace coop::trash_channel {
namespace {

namespace R = ue_wrap::reflection;

// Increment 2 (v84): eid -> the peer slot whose puppet currently holds it via a client-initiated grab.
// The door holdOpen_ analog: a client-initiated grab is EXCLUSIVE (one holder per eid; one held eid per
// peer). Cleared on the land COMMIT (TickCarry -> ClearHeldBy), on the holder's disconnect
// (OnGrabHolderLeft), and on a gross reset (OnDisconnect -> ResetIntentState). HOST-only, game-thread.
std::unordered_map<uint32_t, uint8_t> g_heldBy;

// CLIENT-side carry-state (the E-press grab/throw toggle). A player holds at most one trash clump, so a
// single eid each: g_clientPendingGrab is a GrabIntent in flight (set on send, cleared when the matching
// ToClump confirms or a different convert supersedes); g_clientCarry is the eid we are currently carrying
// (set on the confirming ToClump, cleared on the ToPile land or an optimistic throw send). 0 = none.
uint32_t g_clientPendingGrab = 0;
uint32_t g_clientCarry       = 0;

}  // namespace

// ---- Internal ops for the core (trash_channel_detail.h) ------------------

bool HeldByAny(uint32_t eid) {
    return g_heldBy.find(eid) != g_heldBy.end();
}

void ClearHeldBy(uint32_t eid) {
    g_heldBy.erase(eid);
}

void ResetIntentState() {
    g_heldBy.clear();   // v84: drop all client-grab HELD_BY records
    g_clientPendingGrab = 0;  // v85: drop the client carry-state toggle
    g_clientCarry       = 0;
}

// ---- CLIENT senders + carry-state toggle ----------------------------------

void SendGrabIntent(coop::net::Session& s, uint32_t eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    if (s.role() != coop::net::Role::Client) {
        UE_LOGW("[GRAB-INTENT] SendGrabIntent called on a non-client -- ignoring (host grabs directly)");
        return;
    }
    coop::net::GrabIntentPayload p{};
    p.eid = eid;
    s.SendReliable(coop::net::ReliableKind::GrabIntent, &p, sizeof(p));
    g_clientPendingGrab = eid;   // a request in flight: the matching inbound ToClump confirms the carry
    UE_LOGI("[GRAB-INTENT] CLIENT SENT eid=%u -> host (pending grab)", eid);
}

void SendThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode, const ue_wrap::FVector& dir) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    if (s.role() != coop::net::Role::Client) {
        UE_LOGW("[THROW-INTENT] SendThrowIntent called on a non-client -- ignoring");
        return;
    }
    coop::net::ThrowIntentPayload p{};
    p.eid  = eid;
    p.mode = mode;
    if (mode == coop::net::throw_mode::kHardThrow) { p.dirX = dir.X; p.dirY = dir.Y; p.dirZ = dir.Z; }
    s.SendReliable(coop::net::ReliableKind::ThrowIntent, &p, sizeof(p));
    if (g_clientCarry == eid) g_clientCarry = 0;   // optimistic: the ToPile land will also clear it
    UE_LOGI("[THROW-INTENT] CLIENT SENT eid=%u mode=%s -> host (carry released)",
            eid, mode == coop::net::throw_mode::kHardThrow ? "hardThrow(LMB)" : "release(E)");
}

void NoteClientConvertObserved(uint32_t eid, bool toClump) {
    if (eid == 0u) return;
    if (toClump) {
        if (eid == g_clientPendingGrab) {           // the host confirmed OUR grab -> we are now carrying it
            g_clientCarry = eid;
            g_clientPendingGrab = 0;
            UE_LOGI("[GRAB-INTENT] CLIENT carry CONFIRMED eid=%u (inbound ToClump matched our request)", eid);
        }
    } else {                                        // ToPile (a land): if it is what we carried, the carry ended
        if (eid == g_clientCarry) {
            g_clientCarry = 0;
            UE_LOGI("[THROW-INTENT] CLIENT carry ENDED eid=%u (inbound ToPile -- re-piled)", eid);
        }
        if (eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // a grab that re-piled before we carried -> drop pending
    }
}

coop::element::ElementId ClientCarryEid() {
    return g_clientCarry == 0 ? coop::element::kInvalidId
                              : static_cast<coop::element::ElementId>(g_clientCarry);
}

void ClearClientCarry(uint32_t eid) {
    if (eid != 0 && eid == g_clientCarry) {
        g_clientCarry = 0;
        UE_LOGI("[THROW-INTENT] CLIENT carry CLEARED eid=%u (carried proxy retired -- host aborted the carry)", eid);
    }
    if (eid != 0 && eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // also drop a pending request for a vanished eid
}

// ---- HOST executors --------------------------------------------------------

void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // GATE 1 (door CanOpen analog): already carrying -> mid-hold, deny. The host's PropConvert stream
    // already conveys the true state to the requester; no correction packet needed.
    if (IsCarrying(static_cast<coop::element::ElementId>(eid))) {
        UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- already HELD (carry latch open)", eid, senderSlot);
        return;
    }
    // (No "ctx generation" gate: g_ctx is written only when an eid has ALREADY transitioned (a convert
    // Bumps it). A RESTING pile that has never been grabbed has an eid + proxy but NO ctx entry yet -- which
    // is the COMMON grab case. The real "is this a valid grab target" check is the live-pile resolve below,
    // not g_ctx presence. The 2026-06-22 smoke caught this: a fresh pile (eid 4424) was wrongly DENIED here.)
    // GATE 2 (door per-peer holdOpen_ analog): one held eid per peer. If this slot already holds something,
    // deny (it must release first).
    for (const auto& kv : g_heldBy) {
        if (kv.second == senderSlot) {
            UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- slot already holds eid=%u", eid, senderSlot, kv.first);
            return;
        }
    }

    // Resolve puppet-N + the pile actor.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!puppet) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- puppet not live", eid, senderSlot);
        return;
    }
    // Resolve eid -> the host's pile actor via the canonical Element Registry. IsLiveByIndex-guarded (the
    // cached GUObjectArray slot, NOT a deref of a possibly-GC'd pointer -- the [[feedback-islive-unsafe...]]
    // pattern, same as remote_prop's internal resolver).
    coop::element::Element* pe = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* pa   = pe ? pe->GetActor() : nullptr;
    void* pile = (pa && R::IsLiveByIndex(pa, pe->GetInternalIdx())) ? pa : nullptr;
    if (!pile) {
        // The eid is UNRESOLVABLE on the host (no row / stale-dead actor pointer) while the requester
        // still resolves a mirror to it -- a stale-identity ghost. 2026-07-02 wedge: a GC-churned keyed
        // prop left the eid-2947 row pointing at a freed address; a chipPile RECYCLED that address on
        // the client, the stale reverse entry mis-resolved its aim, and it re-sent the same doomed grab
        // 12x while this deny stayed SILENT -- unwedged only when the host hand-cycled the real pile.
        // Broadcast PropDestroy(eid) instead: positive per-eid host-authoritative evidence (the
        // join-reconcile-sweep-safety shape); every peer DRAINS its row for the eid (a stale row
        // resolves no live actor -> UnregisterPropMirror only, no world actor destroyed; peers without
        // the row no-op in steady state) and the requester's next aim re-resolves to the REAL entity.
        // No transition race: a host-grab-in-flight eid is denied at GATE 1 (g_carry) above, and input
        // dispatch + packet processing are both game-thread-serialized. The host's own corpse row is
        // the reaper's to drain (one owner; this edge only reports the death).
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- eid unresolvable on the host (row=%s actor=%p) -> "
                "broadcasting PropDestroy(eid) so every peer drains its stale ghost row",
                eid, senderSlot, pe ? "stale-dead" : "absent", pa);
        coop::net::PropDestroyPayload dp{};
        dp.key.len   = 0;   // eid-only: mirror rows are eid-keyed on every peer
        dp.elementId = eid;
        s.SendPropDestroy(dp);
        return;
    }
    if (!ue_wrap::prop::IsChipPile(pile)) {
        // A LIVE actor of the wrong class: the identity names a real entity, so NEVER destroy on a
        // class mismatch. But silence leaves the requester WEDGED: its row for this eid resolves a
        // pile-shaped mirror (native hover GUI + aim hit), so it re-sends the same doomed grab
        // forever (19:24 verdict: eid=3129 = trashBitsPile_C here vs a bound native chipPile on the
        // client; 8 identical denies, user: "pile нельзя взять вообще"). Heal by RE-ASSERTING THE
        // TRUTH: broadcast ONE incremental authoritative PropSpawn for the live actor (bracket-free
        // additive; peers re-bind the eid to the real key/class via RegisterPropMirror -- the same
        // morph re-skin path every rebind takes), so the requester's stale pile-row is replaced and
        // its next aim re-resolves. Same deny-owner contract as the not-live branch above
        // (d4833b9b): every terminal deny answers with positive host-authoritative evidence, never
        // silence. Debounced per eid -- spam-pressed E must not spam the wire (the send is
        // idempotent on peers). The smear's UPSTREAM (how one eid came to name different actors on
        // two peers) is the keyed-prop GC-churn re-bind thread (docs/piles/12) -- that root is
        // prevention; this is the deny edge's truth channel.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- live actor %p class '%ls' is not a chipPile "
                "(cross-peer identity smear?) -> re-asserting the authoritative row (incremental "
                "PropSpawn) so the requester re-binds", eid, senderSlot, pile,
                R::ClassNameOf(pile).c_str());
        static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_lastSmearHeal;
        const auto healNow = std::chrono::steady_clock::now();
        auto healIt = s_lastSmearHeal.find(eid);
        if (healIt == s_lastSmearHeal.end() ||
            healNow - healIt->second > std::chrono::seconds(5)) {
            s_lastSmearHeal[eid] = healNow;
            coop::prop_snapshot::ExpressIncrementalSpawn(pile);
        }
        return;
    }

    // EXECUTE the real grab on puppet-N. The PROBE-PROVEN pattern (votv-puppet-grab-feasibility-RE-2026-06-22):
    // pile->playerGrabbed(Player=puppet); HitResult left zeroed (not a gate). The pile self-destructs in the
    // call (K2_DestroyActor(self)) -- do NOT deref `pile` afterwards. playerGrabbed sets the puppet's
    // grabbing_actor SYNCHRONOUSLY (pickupObject has no tick gate, RE-confirmed), so we read it on return.
    // [PILE-TYPE probe 2026-07-04] pos+chipType of the HOST's resolved actor for this eid. Compare
    // against the requester's "CLIENT E-PRESS ... at(...) chipType=" line for the same eid: positions
    // differing = the ordinal identity misalignment (client aimed at a DIFFERENT pile than the host
    // resolves -> the wrong-type clump/morph the 18:45 hands-on saw). Read-only, per-grab cadence.
    {
        const ue_wrap::FVector hloc = ue_wrap::engine::GetActorLocation(pile);
        UE_LOGI("[GRAB-INTENT] EXEC puppet=%p pile=%p eid=%u slot=%u at(%.1f,%.1f,%.1f) chipType=%u",
                puppet, pile, eid, senderSlot, hloc.X, hloc.Y, hloc.Z,
                static_cast<unsigned>(ue_wrap::prop::GetChipType(pile)));
    }
    void* pileCls = R::ClassOf(pile);
    void* grabFn  = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
    if (!grabFn) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- playerGrabbed UFunction not found", eid, senderSlot);
        return;
    }
    {
        ue_wrap::ParamFrame pf(grabFn);
        pf.Set<void*>(L"Player", puppet);
        ue_wrap::Call(pile, pf);   // pile self-destructs HERE; `pile` is now dangling
    }

    // Read the clump the puppet now holds (grabbing_actor, the PHC slot -- the carry-churn RE established the
    // clump rides grabbing_actor, NOT holding_actor).
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* clump = nullptr;
    if (ue_wrap::engine::ReadMainPlayerGrabState(puppet, gs))
        clump = (gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ? gs.grabbingActor : nullptr;
    if (!clump) {
        UE_LOGW("[GRAB-INTENT] eid=%u slot=%u -- playerGrabbed ran but no clump in grabbing_actor (denying)",
                eid, senderSlot);
        return;
    }
    // v106b: consume the birth certificate -- the puppet's hand IS this clump's hand-edge (the
    // owner-side held-edge in local_streams never fires for a puppet grab). Without the consume
    // the certificate expires 60 ticks in and the expiry-express would broadcast a spurious
    // second ToClump at the carried (mid-air) transform.
    {
        coop::element::ElementId bornE = coop::element::kInvalidId;
        uint8_t bornChip = 0;
        TakeClumpBorn(clump, &bornE, &bornChip);
    }

    // L3 (carry-jitter root fix): take the clump OUT of the physics solver for the duration of the carry.
    // playerGrabbed engaged the puppet's PhysicsHandleComponent on a still-SIMULATING body, but the puppet
    // tick never advances the PHC target (probe verdict FLOATING), so the PHC spring (frozen at the grab
    // spot) + gravity FIGHT our per-tick SetActorLocation teleport (puppet_carry_drive) -- the body
    // oscillates, and the host reads that jittered pose back into the carry stream so every peer shakes.
    // A kinematic body honors SetActorLocation exactly, with no spring/gravity to fight, so our drive is the
    // sole authority and the published pose is clean. Re-enabled (false->true) at OnThrowIntent for the arc.
    ue_wrap::engine::SetActorSimulatePhysics(clump, false);

    // Record HELD_BY before the convert (OnHostConvert opens the carry latch). Then convert E onto the clump
    // (bumps ctx + broadcasts PropConvert{kToClump} to ALL incl. the requester) + register the per-tick hand
    // drive (the puppet's own tick won't position the clump -- the probe's verdict).
    g_heldBy[eid] = senderSlot;
    const ue_wrap::FVector  clumpLoc = ue_wrap::engine::GetActorLocation(clump);
    const ue_wrap::FRotator clumpRot = ue_wrap::engine::GetActorRotation(clump);
    const uint8_t chipType = ue_wrap::prop::GetChipType(clump);
    OnHostConvert(s, static_cast<coop::element::ElementId>(eid), coop::net::propconvert_kind::kToClump,
                  clump, clumpLoc, clumpRot, chipType);
    coop::puppet_carry_drive::NotePuppetHeld(static_cast<coop::element::ElementId>(eid), senderSlot, clump);
    UE_LOGI("[GRAB-INTENT] SUCCESS eid=%u clump=%p slot=%u -- ToClump broadcast + hand-drive armed",
            eid, clump, senderSlot);
}

void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // GATE: the sender must currently HOLD this eid (HELD_BY). Else it's a stale/forged throw -> deny.
    auto held = g_heldBy.find(eid);
    if (held == g_heldBy.end() || held->second != senderSlot) {
        UE_LOGI("[THROW-INTENT] DENIED eid=%u slot=%u -- sender does not hold this eid", eid, senderSlot);
        return;
    }
    // Resolve puppet-N + the held clump (E is bound to the clump by OnGrabIntent's OnHostConvert -> RebindE).
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    coop::element::Element* ce = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* ca    = ce ? ce->GetActor() : nullptr;
    void* clump = (ca && R::IsLiveByIndex(ca, ce->GetInternalIdx())) ? ca : nullptr;
    if (!puppet || !clump) {
        UE_LOGW("[THROW-INTENT] eid=%u slot=%u -- puppet/clump not live (puppet=%p clump=%p) -- releasing hold",
                eid, senderSlot, puppet, clump);
        ReleaseClientHold(s, static_cast<coop::element::ElementId>(eid));
        return;
    }

    // RE (votv-puppet-grab-feasibility-RE / agent throw RE): the throw must (1) RELEASE the puppet's grab so
    // the clump's re-pile gate (IsValid(holdPlayer.grabbing_actor)) reads NOT-held -- else it aborts the
    // re-pile; (2) the native release applies NO impulse (the launch is inherited kinematic velocity), but a
    // host-driven clump has none, so the host applies the throw velocity ITSELF along the puppet's synced aim
    // (= where the client looks); (3) hit-notify ON + physics ON so the flying clump generates the ground
    // contact that fires its OWN re-pile ubergraph -> the existing BeginDeferred thunk converts ToPile.
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(puppet, clump);   // clear grabbing_actor + PHC ReleaseComponent
    ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(clump, true);  // re-pile depends on the contact stream
    ue_wrap::engine::SetActorSimulatePhysics(clump, true);
    ue_wrap::engine::SetActorRootCollisionEnabled(clump, /*QueryAndPhysics=*/3);  // collide + land (don't sink)
    // L4 (wild-throw root fix): native E is the ONLY release input and the launch is the body's INHERITED
    // hand/camera motion at release (a still player -> ~0 -> a soft drop; a flick -> a real throw), NOT a
    // fixed impulse. The old constant {aim*600, +400} fired a flat ~871 cm/s on EVERY E = always a hard
    // throw, never a drop. puppet_carry_drive tracks the hold point's smoothed per-tick velocity (the
    // kinematic analog of the native PHC's inherited tracked velocity) -- use THAT, clamped to a brisk-human
    // max (a raw teleport delta on a fast flick can spike far past any real throw; the native PHC spring is
    // damped, so we cap the bound). Direction comes from the actual hand motion, not the aim, so a soft drop
    // falls straight down under gravity while a forward flick flies forward -- exactly the native feel.
    ue_wrap::FVector lin;
    if (mode == coop::net::throw_mode::kHardThrow) {
        // LMB native throw (RE 2026-06-26, throwHoldingProp->traceThrow->throwShit): the launch is the
        // engine projectile-toss suggestion, which round-trips to the seed guess
        //   v = cameraForward * (1000 / d) + playerVelocity,  d = max(objMass, 10) / 15
        //     = cameraForward * (15000 / max(mass,10)) + playerVelocity      [NO cap -- a deliberate throw]
        // The host holds the real clump, so read its TRUE mass (the native uses grabbing_component.GetMass);
        // camFwd is the client's instantaneous camera-forward (Variant B -- flies exactly where it looked at
        // the press); playerVel = the requester puppet's velocity (the native adds the thrower's locomotion).
        const float mass  = ue_wrap::engine::GetActorRootMass(clump);
        const float denom = (mass > 10.f) ? mass : 10.f;   // FMax(objMass, 10) -- a 0/unresolved mass floors to 10
        const float speed = 15000.f / denom;
        const ue_wrap::FVector pv = ue_wrap::engine::GetActorVelocity(puppet);
        lin = ue_wrap::FVector{ camFwd.X * speed + pv.X, camFwd.Y * speed + pv.Y, camFwd.Z * speed + pv.Z };
    } else {
        // E-release (#3): the launch is the puppet's smoothed hand motion (a still hold drops soft, a flick
        // flies), capped to a brisk-human max so a teleport-delta spike isn't a wild throw.
        lin = coop::puppet_carry_drive::HandVelocityForEid(static_cast<coop::element::ElementId>(eid));
        constexpr float kMaxThrowCmS = 650.f;   // ~6.5 m/s -- above this is a teleport-delta artifact, not a human throw
        const float sp2 = lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z;
        if (sp2 > kMaxThrowCmS * kMaxThrowCmS) {
            const float sc = kMaxThrowCmS / std::sqrt(sp2);
            lin.X *= sc; lin.Y *= sc; lin.Z *= sc;
        }
    }
    ue_wrap::engine::SetActorRootPhysicsVelocity(clump, lin, ue_wrap::FVector{0.f, 0.f, 0.f});  // apply AFTER SimulatePhysics(true)
    coop::puppet_carry_drive::NoteThrown(static_cast<coop::element::ElementId>(eid));  // stop hand-drive; stream the flight
    UE_LOGI("[THROW-INTENT] SUCCESS eid=%u slot=%u mode=%s clump=%p -- puppet released + physics thrown vel=(%.0f,%.0f,%.0f); "
            "clump flies + self-re-piles (thunk -> ToPile)", eid, senderSlot,
            mode == coop::net::throw_mode::kHardThrow ? "hardThrow(LMB)" : "release(E)", clump, lin.X, lin.Y, lin.Z);
}

void OnGrabHolderLeft(uint8_t senderSlot) {
    for (auto it = g_heldBy.begin(); it != g_heldBy.end(); ) {
        if (it->second == senderSlot) {
            UE_LOGI("[GRAB-INTENT] OnGrabHolderLeft slot=%u -- clearing HELD_BY eid=%u + ForgetEid",
                    senderSlot, it->first);
            ForgetEid(static_cast<coop::element::ElementId>(it->first));  // drop a stranded carry latch/settle
            it = g_heldBy.erase(it);
        } else { ++it; }
    }
}

void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E) {
    const uint32_t eid = static_cast<uint32_t>(E);
    if (g_heldBy.erase(eid))
        UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- clump lost before land; hold cleared (re-grabbable)", eid);
    ForgetEid(E);   // drop a stranded carry latch/settle (idempotent if the land COMMIT already closed it)
    // Audit HIGH 2026-06-23: the trash entity vanished on the host (clump died with no re-pile). Broadcast
    // PropDestroy(eid) so EVERY client retires the now-frozen carry proxy + clears its g_clientCarry toggle
    // (OnDestroy -> trash_proxy::RetireProxy [drive cleared] + ClearClientCarry). Without this the requester
    // is stuck in throw-mode for the dead eid forever (every E-press denied) and its proxy floats in mid-air.
    coop::net::PropDestroyPayload dp{};
    dp.key.len    = 0;            // eid-only: clients resolve the proxy by host-range eid
    dp.elementId  = eid;
    s.SendPropDestroy(dp);
    UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- PropDestroy(eid) broadcast (carry ABORT: clients retire "
            "the frozen proxy + clear the carry toggle)", eid);
}

}  // namespace coop::trash_channel
