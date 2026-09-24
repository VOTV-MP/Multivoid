// coop/props/trash_grab_intent.cpp -- the client-initiated grab and throw intent lane of
// coop::trash_channel (see coop/props/trash_channel.h). This file owns the intent lane's whole
// state: the holder registry (eid to holder peer slot, the door hold's analog; the core reaches
// it through the ops in trash_channel_detail.h) and the client-side pending-grab and
// carry toggles. The core (the context generations, the carry latch, the land settle, the
// birth certificates, the tick) stays in trash_channel.cpp; its carry-latch read here goes
// through the public IsCarrying.

#include "coop/props/trash_channel.h"

#include "trash_channel_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/intent_authority.h"   // may this sender name this pile
#include "coop/element/registry.h"    // resolve the pile to grab
#include "coop/player/players_registry.h"    // the client-grab direction's puppet
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
#include <cmath>      // clamp the inherited throw velocity
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::trash_channel {
namespace {

namespace R = ue_wrap::reflection;

// Eid to the peer slot whose puppet holds it through a client-initiated grab, the door hold's
// analog: a client grab is exclusive, one holder per eid and one held eid per peer. A hold spans
// the grab to the let-go (the throw, a takeover, the holder gone), not to the land: from the
// let-go on the open carry latch alone keeps the eid ungrabbable. Host only, game thread.
std::unordered_map<uint32_t, uint8_t> g_heldBy;

// The client-side carry state, the use-press grab-or-throw toggle. A player holds at most one
// trash clump, so a single eid each: a grab request in flight (set on send, cleared when the
// matching to-clump confirms or a different convert supersedes), and the eid we are carrying
// (set on the confirming to-clump, cleared on the to-pile land or an optimistic throw send).
// 0 means none.
uint32_t g_clientPendingGrab = 0;
uint32_t g_clientCarry       = 0;
uint16_t g_clientGrabReq     = 0;   // the client's request counter: a GrabRefused answers one request, by this number

}  // namespace

// The internal ops for the core (trash_channel_detail.h).

bool HeldByAny(uint32_t eid) {
    return g_heldBy.find(eid) != g_heldBy.end();
}

void ClearHeldBy(uint32_t eid) {
    g_heldBy.erase(eid);
}

// The holder is told, alone, that the host ended its carry: nothing else on the wire says so (a
// takeover sends no convert, a let-go clump may never land), and a client that still believes it
// carries spends its next use press on a throw of a clump it no longer holds. A slot that has
// left is not sent to: the send refuses a slot that is not world-ready.
static void TellHoldEnded(coop::net::Session& s, uint8_t slot, uint32_t eid) {
    coop::net::GrabRefusedPayload p{};
    p.eid    = eid;
    p.reason = static_cast<uint8_t>(coop::net::GrabRefusedReason::HoldEnded);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::GrabRefused, &p, sizeof(p));
}

void EndHoldTakenOver(coop::net::Session& s, uint32_t eid) {
    auto it = g_heldBy.find(eid);
    if (it == g_heldBy.end()) return;
    const uint8_t slot = it->second;
    g_heldBy.erase(it);
    TellHoldEnded(s, slot, eid);
    UE_LOGI("[GRAB-INTENT] eid=%u TAKEN OVER from slot=%u -- hold ended, the holder told", eid, slot);
}

void ResetIntentState() {
    g_heldBy.clear();   // drop all client-grab holds
    g_clientPendingGrab = 0;  // drop the client carry-state toggle
    g_clientCarry       = 0;
}

// The client senders and the carry-state toggle.

void SendGrabIntent(coop::net::Session& s, uint32_t eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    if (s.role() != coop::net::Role::Client) {
        UE_LOGW("[GRAB-INTENT] SendGrabIntent called on a non-client -- ignoring (host grabs directly)");
        return;
    }
    coop::net::GrabIntentPayload p{};
    p.eid   = eid;
    p.reqId = ++g_clientGrabReq;
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
        UE_LOGI("[THROW-INTENT] CLIENT carry CLEARED eid=%u (carried mirror retired -- host aborted the carry)", eid);
    }
    if (eid != 0 && eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // also drop a pending request for a vanished eid
}

void OnGrabRefused(uint32_t eid, uint8_t reason, uint16_t reqId) {
    if (reason == static_cast<uint8_t>(coop::net::GrabRefusedReason::HoldEnded)) {
        // Not an answer to a request: the host ended our carry (its hand or a broom took the
        // clump, or we fell and our puppet let it go).
        // A grab of ours still unanswered goes with it: this notice rides a faster lane than the
        // ToClump that would confirm it, and that late confirmation must not start a carry the host
        // has already ended.
        const bool carried = (eid != 0u && eid == g_clientCarry);
        if (carried) g_clientCarry = 0;
        if (eid != 0u && eid == g_clientPendingGrab) g_clientPendingGrab = 0;
        UE_LOGI("[GRAB-INTENT] CLIENT carry ENDED BY THE HOST eid=%u -- %s", eid,
                carried ? "carry ended, the next press grabs" : "not what we carry -- ignored");
        return;
    }
    // The LATEST request only: a refusal of an earlier request for this same eid must not end a
    // later one the host is about to perform, or the host would hold a clump for a client that
    // believes it carries nothing.
    const bool ours = (eid != 0u && eid == g_clientPendingGrab && reqId == g_clientGrabReq);
    if (ours) g_clientPendingGrab = 0;
    UE_LOGI("[GRAB-INTENT] CLIENT grab REFUSED eid=%u reason=%u req=%u -- %s", eid,
            static_cast<unsigned>(reason), static_cast<unsigned>(reqId),
            ours ? "pending grab cleared" : "not the request in flight (an earlier one, or none) -- ignored");
}

// The host executors.

// Host: answer a refused grab, to the requester alone. Without it the requester's pending grab
// outlives the refusal, and the next ToClump for that eid -- the host's grab, a third peer's --
// reads as its own confirmation: it believes it carries a clump it does not hold, and its use
// presses become throw intents. Reported from the field in pull request 29.
static void Refuse(coop::net::Session& s, uint8_t slot, uint32_t eid, uint16_t reqId,
                   coop::net::GrabRefusedReason reason) {
    coop::net::GrabRefusedPayload p{};
    p.eid    = eid;
    p.reason = static_cast<uint8_t>(reason);
    p.reqId  = reqId;
    s.SendReliableToSlot(slot, coop::net::ReliableKind::GrabRefused, &p, sizeof(p));
}

static void LetGo(uint32_t eid, void* puppet, void* clump);   // below: the one release of a puppet-held clump

void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint16_t reqId, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // Gate 1, the door can-open analog: already carrying means mid-hold, so deny. Every refusal
    // below is ANSWERED (Refuse): the convert stream tells the requester what the pile is, never
    // that its own request is over.
    if (IsCarrying(static_cast<coop::element::ElementId>(eid))) {
        UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- already HELD (carry latch open)", eid, senderSlot);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::AlreadyHeld);
        return;
    }
    // There is no context-generation gate: the context is written only when an eid has already
    // transitioned, and a resting pile that has never been grabbed has an eid and a mirror but no
    // context entry yet, the common grab case; the real is-this-a-valid-target check is the
    // live-pile resolve below. Gate 2, the door per-peer hold analog: one held eid per peer, so a
    // slot that already holds something is denied until it releases.
    for (const auto& kv : g_heldBy) {
        if (kv.second == senderSlot) {
            UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- slot already holds eid=%u", eid, senderSlot, kv.first);
            Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::SlotBusy);
            return;
        }
    }

    // Resolve the puppet and the pile actor.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!puppet || rp->IsRagdollDisplayed()) {
        // No hand to hold with: no puppet, or a fallen one (the carry drive would let go a tick
        // later, and the pile would have been turned into a dropped clump for nothing).
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- puppet not live, or fallen", eid, senderSlot);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::PuppetGone);
        return;
    }
    // Resolve the eid to the host's pile actor and ask whether this sender may name it. The
    // authorizer returns an outcome rather than a pointer because this lane branches three ways
    // with opposite remedies on what a pointer-returning resolver collapses into one null, and its
    // wrong-class branch consumes the actor. The order is load-bearing: the puppet check above
    // runs first, since reversing it would let a sender with no live puppet reach the ghost-heal
    // broadcast below, a host-authored destroy for a client-named eid, the confused-deputy shape.
    // The reach is the cone the client-side producer itself suppresses outside; a refusal here
    // costs a retry and never an item, the pile staying exactly where it is, which is what makes
    // this lane safe to gate.
    constexpr float kGrabReachUU = 400.0f;
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, senderSlot, kGrabReachUU);
    const coop::element::IntentSubject sub =
        tok.Resolve(static_cast<coop::element::ElementId>(eid), coop::element::ElementType::Prop);

    if (sub.outcome == coop::element::IntentOutcome::OutOfReach ||
        sub.outcome == coop::element::IntentOutcome::NoBody) {
        // Not a ghost and not a smear: the eid names a real, live pile the sender is simply not
        // standing near. Nothing to heal and nothing to destroy; broadcasting either would answer a
        // reach question with an identity remedy.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- REASON=%s (dist=%.0f allowed=%.0f); the pile "
                "is real and untouched, the sender is just not near it",
                eid, senderSlot, coop::element::OutcomeName(sub.outcome), sub.distUU, sub.reachUU);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::OutOfReach);
        return;
    }

    void* pile = sub.actor;
    if (sub.outcome == coop::element::IntentOutcome::NoRow ||
        sub.outcome == coop::element::IntentOutcome::StaleDead) {
        // The eid is unresolvable on the host (no row, or a stale dead actor pointer) while the
        // requester still resolves a mirror to it, a stale-identity ghost: a collected keyed prop
        // once left a row pointing at a freed address, a chipPile recycled that address on the
        // client, the stale reverse entry mis-resolved its aim, and it re-sent the same doomed grab
        // while the deny stayed silent. Broadcast a destroy for the eid instead, positive per-eid
        // host-authoritative evidence: every peer drains its row for the eid (a stale row resolves
        // no live actor, so only the mirror registration goes and no world actor is destroyed;
        // peers without the row no-op) and the requester's next aim re-resolves to the real entity.
        // No transition race: a host-grab-in-flight eid is denied at gate 1, and input dispatch and
        // packet processing are both game-thread-serialised. The host's own corpse row is the
        // reaper's to drain; this edge only reports the death. The outcome name distinguishes
        // no-row from stale-dead, which is what a dead pointer used to be printed for.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- eid unresolvable on the host (%s) -> "
                "broadcasting PropDestroy(eid) so every peer drains its stale ghost row",
                eid, senderSlot, coop::element::OutcomeName(sub.outcome));
        coop::net::PropDestroyPayload dp{};
        dp.key.len   = 0;   // eid-only: mirror rows are eid-keyed on every peer
        dp.elementId = eid;
        s.SendPropDestroy(dp);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::Unresolvable);
        return;
    }
    // The wrong-type outcome is checked first and short-circuits: an eid naming a non-prop Element
    // still names a real entity, and the heal below consumes it.
    if (sub.outcome == coop::element::IntentOutcome::WrongType || !ue_wrap::prop::IsChipPile(pile)) {
        // A live actor of the wrong class: the identity names a real entity, so never destroy on a
        // class mismatch. But silence leaves the requester wedged: its row for this eid resolves a
        // pile-shaped mirror (the native hover UI and the aim hit), so it re-sends the same doomed
        // grab forever. Heal by re-asserting the truth: broadcast one incremental authoritative
        // spawn for the live actor (bracket-free, additive; peers re-bind the eid to the real key
        // and class through the mirror registration, the same re-skin path every rebind takes), so
        // the requester's stale pile row is replaced and its next aim re-resolves. Every terminal
        // deny answers with positive host-authoritative evidence, never silence. Debounced per eid,
        // since a spam-pressed key must not spam the wire (the send is idempotent on peers). The
        // smear's upstream, how one eid came to name different actors on two peers, is the
        // keyed-prop re-bind under GC churn; that root is prevention, and this is the deny edge's
        // truth channel.
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
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::NotAPile);
        return;
    }

    // Execute the real grab on the puppet: the pile's playerGrabbed with the puppet as the player,
    // the hit result left zeroed (not a gate). The pile self-destructs in the call, so `pile` is
    // not dereferenced afterwards. playerGrabbed sets the puppet's grabbing_actor synchronously,
    // so it is read on return. The log line carries the host's resolved position and chip type for
    // this eid, to compare against the requester's press line for the same eid: differing
    // positions mean the client aimed at a different pile than the host resolves. Read-only, once
    // per grab. The pose is also the clump's birthplace, its fallback below.
    ue_wrap::FVector hloc{};
    ue_wrap::FRotator hrot{};
    const bool hlocRead = ue_wrap::engine::TryGetActorLocation(pile, hloc);
    const bool hposeRead = hlocRead && ue_wrap::engine::TryGetActorRotation(pile, hrot);
    UE_LOGI("[GRAB-INTENT] EXEC puppet=%p pile=%p eid=%u slot=%u at(%.1f,%.1f,%.1f)%s chipType=%u",
            puppet, pile, eid, senderSlot, hloc.X, hloc.Y, hloc.Z, hlocRead ? "" : " (unread)",
            static_cast<unsigned>(ue_wrap::prop::GetChipType(pile)));
    void* pileCls = R::ClassOf(pile);
    void* grabFn  = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
    if (!grabFn) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- playerGrabbed UFunction not found", eid, senderSlot);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::NoVerb);
        return;
    }
    {
        ue_wrap::ParamFrame pf(grabFn);
        pf.Set<void*>(L"Player", puppet);
        ue_wrap::Call(pile, pf);   // pile self-destructs HERE; `pile` is now dangling
    }

    // Read the clump the puppet now holds: grabbing_actor, the physics-handle slot, not
    // holding_actor.
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* clump = nullptr;
    if (ue_wrap::engine::ReadMainPlayerGrabState(puppet, gs))
        clump = (gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ? gs.grabbingActor : nullptr;
    if (!clump) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- playerGrabbed ran but no clump in grabbing_actor",
                eid, senderSlot);
        Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::NoClump);
        return;
    }
    // The convert below places the clump for every peer. The verb has run -- the pile is gone and
    // the clump is in the puppet's handle -- so an unreadable clump is placed as it was born, at the
    // pile's pose before the verb; with neither, the handle lets go before the refusal.
    ue_wrap::FVector clumpLoc{};
    ue_wrap::FRotator clumpRot{};
    if (!ue_wrap::engine::TryGetActorLocation(clump, clumpLoc) || !ue_wrap::engine::TryGetActorRotation(clump, clumpRot)) {
        if (!hposeRead) {
            UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- neither the clump nor the pile before it could be read; "
                    "the puppet's handle lets go", eid, senderSlot);
            LetGo(eid, puppet, clump);
            Refuse(s, senderSlot, eid, reqId, coop::net::GrabRefusedReason::NoClump);
            return;
        }
        UE_LOGW("[GRAB-INTENT] eid=%u slot=%u -- the clump's location or rotation could not be read; converted at "
                "its birthplace", eid, senderSlot);
        clumpLoc = hloc;
        clumpRot = hrot;
    }
    // Consume the birth certificate: the puppet's hand is this clump's hand edge (the owner-side
    // held edge never fires for a puppet grab). Without the consume the certificate expires
    // later, and the expiry express would broadcast a spurious second to-clump at the carried,
    // mid-air transform.
    {
        coop::element::ElementId bornE = coop::element::kInvalidId;
        uint8_t bornChip = 0;
        TakeClumpBorn(clump, &bornE, &bornChip);
    }

    // The clump STAYS in the physics solver, on the puppet's own physics handle, which playerGrabbed
    // just engaged. The puppet's tick never advances the handle's target, so the carry drive does
    // (coop/player/puppet_carry_drive): that is the whole of what a puppet's hold lacks. A kinematic
    // clump moved by location writes looked clean and shoved every box it touched with no force
    // limit, where the game's hold is a spring that a heavy box stops.

    // Record the holder before the convert (OnHostConvert opens the carry latch). Then convert E
    // onto the clump (the context bump and the to-clump broadcast to all, the requester included)
    // and register the per-tick hand drive, since the puppet's own tick will not position the
    // clump.
    g_heldBy[eid] = senderSlot;
    const uint8_t chipType = ue_wrap::prop::GetChipType(clump);
    OnHostConvert(s, static_cast<coop::element::ElementId>(eid), coop::net::propconvert_kind::kToClump,
                  clump, clumpLoc, clumpRot, chipType);
    coop::puppet_carry_drive::NotePuppetHeld(static_cast<coop::element::ElementId>(eid), senderSlot, clump, clumpRot);
    UE_LOGI("[GRAB-INTENT] SUCCESS eid=%u clump=%p slot=%u -- ToClump broadcast + hand-drive armed",
            eid, clump, senderSlot);
}

// The hold on `eid` ends and its clump lives on: a throw, the holder gone. The puppet's handle lets
// go (clearing grabbing_actor, which the clump's re-pile gate reads as its holder's hand), the
// clump collides and reports its hits (its re-pile is fired by the ground contact), the holder
// registry forgets it, and the drive stops steering and streams the free body until the land or the
// rest closes the lane. The hold ends HERE and not at the land: a clump that never re-piles (it
// came to rest on a simulating box, on a slope, before it armed) has no land, the rest closer skips
// a held eid, and a hold kept that long refused its holder every later grab. The carry latch,
// still open, is what keeps the eid ungrabbable in flight. The native release applies no impulse:
// the body leaves the handle with the velocity the hold gave it.
static void LetGo(uint32_t eid, void* puppet, void* clump) {
    if (puppet) ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(puppet, clump);
    ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(clump, true);
    ue_wrap::engine::SetActorRootCollisionEnabled(clump, /*QueryAndPhysics=*/3);
    g_heldBy.erase(eid);
    coop::puppet_carry_drive::NoteLetGo(static_cast<coop::element::ElementId>(eid));
}

void OnHolderGone(coop::net::Session& s, coop::element::ElementId E) {
    const uint32_t eid = static_cast<uint32_t>(E);
    auto held = g_heldBy.find(eid);
    if (held == g_heldBy.end()) return;
    TellHoldEnded(s, held->second, eid);
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(held->second);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    coop::element::Element* ce = coop::element::Registry::Get().Get(E);
    void* ca = ce ? ce->GetActor() : nullptr;
    if (ca && R::IsLiveByIndex(ca, ce->GetInternalIdx()) && ue_wrap::prop::IsGarbageClump(ca)) {
        UE_LOGI("[GRAB-INTENT] eid=%u slot=%u -- the holder cannot hold (left, fell, no puppet): the clump "
                "is let go where it is (it falls and lands like a release)", eid, held->second);
        LetGo(eid, puppet, ca);
    } else {
        // No clump to let go of. The lane stays: a settle in flight commits the land, and a clump
        // that died with none is the carry termination's to report.
        g_heldBy.erase(held);
    }
}

void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // The gate: the sender must currently hold this eid; otherwise a stale or forged throw, denied.
    auto held = g_heldBy.find(eid);
    if (held == g_heldBy.end() || held->second != senderSlot) {
        UE_LOGI("[THROW-INTENT] DENIED eid=%u slot=%u -- sender does not hold this eid", eid, senderSlot);
        return;
    }
    // Resolve the puppet and the held clump; E was bound to the clump by the grab's convert.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    coop::element::Element* ce = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* ca    = ce ? ce->GetActor() : nullptr;
    void* clump = (ca && R::IsLiveByIndex(ca, ce->GetInternalIdx()) && ue_wrap::prop::IsGarbageClump(ca))
                      ? ca : nullptr;
    if (!clump) {
        UE_LOGW("[THROW-INTENT] eid=%u slot=%u -- clump not live -- releasing hold", eid, senderSlot);
        ReleaseClientHold(s, static_cast<coop::element::ElementId>(eid));
        return;
    }
    if (!puppet) {
        OnHolderGone(s, static_cast<coop::element::ElementId>(eid));   // a live clump is let go, never retired
        return;
    }

    LetGo(eid, puppet, clump);
    ue_wrap::FVector lin = ue_wrap::engine::GetActorVelocity(clump);   // what the hold gave it: the release keeps it
    if (mode == coop::net::throw_mode::kHardThrow) {
        // The native hard throw: the launch is the engine's projectile-toss suggestion, which
        // reduces to camera-forward times 15000 over the clump's mass floored at 10, plus the
        // thrower's velocity, with no cap, a deliberate throw. The host holds the real clump, so it
        // reads the true mass; the camera forward is the client's at the press, so the clump flies
        // exactly where it looked; the puppet's velocity stands in for the thrower's locomotion.
        // That forward is a client's word and becomes a velocity on a host body: one that is not
        // finite, or not a direction, falls back to where the host sees the puppet aim.
        ue_wrap::FVector dir = camFwd;
        const float dirLen = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
        if (std::isfinite(dirLen) && dirLen > 1.0e-3f)
            dir = ue_wrap::FVector{dir.X / dirLen, dir.Y / dirLen, dir.Z / dirLen};
        else
            dir = rp->GetSyncedAimDirection();
        const float mass  = ue_wrap::engine::GetActorRootMass(clump);
        const float denom = (mass > 10.f) ? mass : 10.f;   // FMax(objMass, 10) -- a 0/unresolved mass floors to 10
        const float speed = 15000.f / denom;
        const ue_wrap::FVector pv = ue_wrap::engine::GetActorVelocity(puppet);
        lin = ue_wrap::FVector{ dir.X * speed + pv.X, dir.Y * speed + pv.Y, dir.Z * speed + pv.Z };
        ue_wrap::engine::SetActorRootPhysicsVelocity(clump, lin, ue_wrap::FVector{0.f, 0.f, 0.f});
    }
    UE_LOGI("[THROW-INTENT] SUCCESS eid=%u slot=%u mode=%s clump=%p -- puppet released + physics thrown vel=(%.0f,%.0f,%.0f); "
            "clump flies + self-re-piles (thunk -> ToPile)", eid, senderSlot,
            mode == coop::net::throw_mode::kHardThrow ? "hardThrow(LMB)" : "release(E)", clump, lin.X, lin.Y, lin.Z);
}

void OnGrabHolderLeft(coop::net::Session& s, uint8_t senderSlot) {
    // Collected first: letting go edits the registry this walks.
    std::vector<uint32_t> eids;
    for (const auto& kv : g_heldBy)
        if (kv.second == senderSlot) eids.push_back(kv.first);
    for (uint32_t eid : eids) OnHolderGone(s, static_cast<coop::element::ElementId>(eid));
}

void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E) {
    const uint32_t eid = static_cast<uint32_t>(E);
    if (g_heldBy.erase(eid))
        UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- clump lost before land; hold cleared (re-grabbable)", eid);
    ForgetEid(E);   // drop a stranded carry latch/settle (idempotent if the land COMMIT already closed it)
    // The trash entity vanished on the host (the clump died with no re-pile). Broadcast a destroy
    // for the eid, so every client retires the now-frozen carry mirror and clears its carry toggle
    // (the destroy receiver retires the mirror and clears the carry). Without it the requester is
    // stuck in throw mode for the dead eid forever, every press denied, and its mirror floats in
    // mid-air.
    coop::net::PropDestroyPayload dp{};
    dp.key.len    = 0;            // eid-only: clients resolve the mirror by host-range eid
    dp.elementId  = eid;
    s.SendPropDestroy(dp);
    UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- PropDestroy(eid) broadcast (carry ABORT: clients retire "
            "the frozen mirror + clear the carry toggle)", eid);
}

}  // namespace coop::trash_channel
