// coop/trash_channel.cpp -- see coop/trash_channel.h.

#include "coop/props/trash_channel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/registry.h"    // Registry::Get(eid) -- resolve the pile to grab (Increment 2)
#include "coop/player/players_registry.h"    // Puppet(slot) -- the client-grab direction (Increment 2)
#include "coop/player/puppet_carry_drive.h"  // NotePuppetHeld -- host drives the puppet-held clump pose
#include "coop/player/remote_player.h"       // RemotePlayer::GetActor / valid
#include "coop/props/remote_prop.h"   // RegisterPropMirror (the single rebind entry point)
#include "coop/session/save_transfer.h"  // docs/piles/09: TryGetSaveTimePileXformAnySlot (kToPile save-time key)
#include "ue_wrap/call.h"        // ParamFrame / Call (the probe-proven puppet-grab pattern)
#include "ue_wrap/engine.h"      // GetActorScale3D (v83 per-form proxy scale) + ReadMainPlayerGrabState
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"        // IsChipPile / IsGarbageClump / GetChipType
#include "ue_wrap/reflection.h"  // ClassNameOf / ClassOf / FindFunction
#include "ue_wrap/types.h"       // FVector / FRotator / NormalizeAxis

#include <cmath>      // std::sqrt -- clamp the inherited throw velocity (L4)
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::trash_channel {
namespace {

namespace R = ue_wrap::reflection;

// Per-eid sync-time-context. HOST: the AUTHORITY (BumpCtx writes via OnHostConvert -- grab/land converts).
// CLIENT: a pure MIRROR (AdoptInboundConvertCtx writes). Both are GAME-THREAD-only. The two never run
// on the same machine for the same eid (the host authors, clients mirror), so there is no contention.
std::unordered_map<uint32_t, uint8_t> g_ctx;  // eid -> current generation (0 = never transitioned)

// Wrap-aware generation compare: is `a` newer-than-or-equal-to `b`? A uint8 ctx won't realistically wrap
// within one eid's grab sequence, but the signed-difference test is correct across the 255->0 boundary.
inline bool AtLeast(uint8_t a, uint8_t b) {
    return static_cast<int8_t>(static_cast<uint8_t>(a - b)) >= 0;
}

// Advance E's generation, skipping the 0 "no enforcement" sentinel on wrap.
uint8_t Bump(uint32_t E) {
    uint8_t& c = g_ctx[E];
    c = static_cast<uint8_t>(c + 1);
    if (c == 0) c = 1;
    return c;
}

// Re-skin eid E onto `actor` in place (the salvaged morph primitive). RegisterPropMirror is the single
// rebind entry point -- it routes on the Element's authoritative IsMirror() flag (a MIRROR -> SetActor;
// a host's OWN local element -> RebindLocalElementActor, keeping the forward map consistent), so it is
// correct for a host re-pointing its own pile/clump.
void RebindE(coop::element::ElementId E, void* actor) {
    coop::remote_prop::RegisterPropMirror(E, actor, L"", R::ClassNameOf(actor), /*senderSlot=*/0,
                                          /*rebindInPlace=*/true);
}

// The pending grab recorded at the InpActEvt_use PRE seam, consumed by the next held-clump edge.
struct PendingGrab { bool active = false; uint32_t eid = 0; uint8_t chipType = 0; int ttl = 0; };
PendingGrab g_pendingGrab;
constexpr int kPendingGrabTtlTicks = 30;  // ~0.3-0.5s; the grab's clump enters the hand within a few frames

// ---- CLOSE-B carry latch + land-settle (host-side) -- see trash_channel.h for the full model ----------
std::unordered_map<uint32_t, uint32_t> g_carry;   // eid -> last-activity tick; PRESENCE in the map = carrying
struct LandSettle {
    int               countdown;  // ticks until COMMIT (a re-grab CANCELS first)
    int               sincePile;  // ticks since the held ToPile (the ToPile->ToClump gap, logged on CANCEL)
    uint8_t           chipType;
    // take-31: the spawned pile actor + its GUObjectArray index. At the BeginDeferred POST the pile is NOT yet
    // positioned (FinishSpawning runs later), so the loc/rot/scale captured then are the CLUMP fallback. At the
    // COMMIT (K ticks later, post-FinishSpawning) we RE-READ the pile's REAL transform via this pointer
    // (IsLiveByIndex-guarded -- it is cached across the multi-tick settle). A committing settle (no re-grab
    // within K) always has a live, settled pile, so the re-read is the authoritative source; loc/rot/scale
    // below are the fallback if the pile somehow died.
    void*             pileActor = nullptr;
    int32_t           pileIdx   = -1;
    ue_wrap::FVector  scale{1.f, 1.f, 1.f};  // FALLBACK scale (clump/default at the thunk; re-read at COMMIT)
    ue_wrap::FVector  loc;                    // FALLBACK loc  (the clump's -- ~radius high; re-read at COMMIT)
    ue_wrap::FRotator rot;                    // FALLBACK rot  (the clump's tumble; re-read at COMMIT)
    std::string       cls;        // the pile class to broadcast at COMMIT
};
std::unordered_map<uint32_t, LandSettle> g_settle;

// Increment 2 (v84): eid -> the peer slot whose puppet currently holds it via a client-initiated grab.
// The door holdOpen_ analog: a client-initiated grab is EXCLUSIVE (one holder per eid; one held eid per
// peer). Cleared on the land COMMIT (TickCarry), on the holder's disconnect (OnGrabHolderLeft), and on
// a gross reset (OnDisconnect). HOST-only, game-thread.
std::unordered_map<uint32_t, uint8_t> g_heldBy;

// CLIENT-side carry-state (the E-press grab/throw toggle). A player holds at most one trash clump, so a
// single eid each: g_clientPendingGrab is a GrabIntent in flight (set on send, cleared when the matching
// ToClump confirms or a different convert supersedes); g_clientCarry is the eid we are currently carrying
// (set on the confirming ToClump, cleared on the ToPile land or an optimistic throw send). 0 = none.
uint32_t g_clientPendingGrab = 0;
uint32_t g_clientCarry       = 0;

uint32_t g_tick = 0;
constexpr int kLandSettleTicks = 6;   // K: > a synchronous churn re-grab (~1 frame). TUNE from the CANCEL-gap log.

std::string NarrowAscii(const std::wstring& w) {          // BP class names are ASCII -> lossless narrowing.
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

// The SINGLE PropConvert send primitive (bumps ctx). OnHostConvert calls it for the OPEN (real grab) + the
// not-carrying land; TickCarry calls it for the settle COMMIT (the real land). `why` is a log tag.
uint8_t BroadcastConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind,
                         const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                         const ue_wrap::FVector& scale, uint8_t chipType, const std::string& cls,
                         const char* why) {
    const uint8_t ctx = Bump(static_cast<uint32_t>(E));
    coop::net::PropConvertPayload p{};
    p.oldEid = static_cast<uint32_t>(E);                  // bind model: oldEid == newEid == E
    p.newEid = static_cast<uint32_t>(E);
    p.pileClass.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) p.pileClass.data[p.pileClass.len++] = cls[i];
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v83: the host's real per-form scale (a clump and a pile differ) so the proxy is host-sized.
    p.scaleX = scale.X; p.scaleY = scale.Y; p.scaleZ = scale.Z;
    p.chipType = chipType;
    p.kind     = kind;
    p.ctx      = ctx;
    // docs/piles/09: a kToPile LAND carries the pile's PRE-GRAB save-time position IF one was recorded
    // (the host grabbed an UNTRACKED pile in a join window -> OnPileGrabPre self-seeded the eid +
    // RecordGrabTimePileXform stamped the pre-grab pos into the blob map). ANY-SLOT lookup (this is a
    // single fan-out; the eid is unique). The client OnConvert then arms a pending save-time twin so the
    // quiescence sweep retires its stale native@old. kToClump carries no key (no native twin at that edge).
    p.hasMatchPos = 0;
    if (kind == coop::net::propconvert_kind::kToPile && static_cast<uint32_t>(E) != 0u) {
        ue_wrap::FVector sv;
        if (coop::save_transfer::TryGetSaveTimePileXformAnySlot(E, sv)) {
            p.matchX = sv.X; p.matchY = sv.Y; p.matchZ = sv.Z;
            p.hasMatchPos = 1;
        }
    }
    s.SendReliable(coop::net::ReliableKind::PropConvert, &p, sizeof(p));
    UE_LOGI("[TRASH-CH] HOST BROADCAST %s eid=%u ctx=%u (%s) at (%.1f,%.1f,%.1f) variant=%u%s",
            kind == coop::net::propconvert_kind::kToClump ? "ToClump" : "ToPile",
            static_cast<unsigned>(E), static_cast<unsigned>(ctx), why,
            p.locX, p.locY, p.locZ, static_cast<unsigned>(chipType),
            p.hasMatchPos ? " [+saveTimeKey docs/piles/09]" : "");
    return ctx;
}

// CANCEL E's pending land-settle (a re-grab arrived -> the preceding re-pile was churn, not the land). Logs
// the ToPile->ToClump gap (= the churn re-grab latency) -- READ THIS to tune kLandSettleTicks.
void CancelSettle(uint32_t eid, const char* why) {
    auto it = g_settle.find(eid);
    if (it == g_settle.end()) return;
    UE_LOGI("[TRASH-CH] HOST land-settle CANCEL eid=%u gap=%d ticks (%s) -- ToPile->ToClump latency (tune K from this)",
            eid, it->second.sincePile, why);
    g_settle.erase(it);
}

}  // namespace

void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType) {
    if (E == 0u || E == coop::element::kInvalidId || !newActor) return;
    const uint32_t eid     = static_cast<uint32_t>(E);
    const bool     toClump = (kind == coop::net::propconvert_kind::kToClump);
    const bool     carrying = (g_carry.find(eid) != g_carry.end());

    RebindE(E, newActor);                                 // ALWAYS track the live actor locally (keep the rebind)
    const std::string cls = NarrowAscii(R::ClassNameOf(newActor));
    const ue_wrap::FVector scale = ue_wrap::engine::GetActorScale3D(newActor);  // v83: the new form's real scale

    if (toClump) {
        if (!carrying) {
            // OPEN: the REAL grab (pile->clump, via AdoptPendingGrabClump). Broadcast the one ToClump + start
            // the carry session; the host's churn (re-pile + auto-re-grab) is SUPPRESSED until the real land.
            g_carry[eid] = g_tick;
            BroadcastConvert(s, E, kind, loc, rot, scale, chipType, cls, "GRAB OPEN");
            UE_LOGI("[TRASH-CH] HOST carry OPEN eid=%u -- churn re-pile/re-grab suppressed until the land", eid);
        } else {
            // Defensive: a ToClump reaching OnHostConvert while carrying (the held-edge OnHostRegrab is the
            // normal re-grab path; the thunk skips pile-source so it should not). Fold as churn -- rebind
            // (done), cancel any pending settle, suppress the broadcast + ctx bump.
            g_carry[eid] = g_tick;
            CancelSettle(eid, "re-grab(convert)");
            UE_LOGI("[TRASH-CH] HOST SUPPRESS ToClump eid=%u (carry re-grab) -- rebound, no broadcast/ctx", eid);
        }
    } else {  // kToPile
        if (!carrying) {
            // A re-pile we are NOT tracking as a carry (an ambient/untracked clump, or a land after we already
            // closed) -- no churn to fold -> broadcast immediately.
            BroadcastConvert(s, E, kind, loc, rot, scale, chipType, cls, "LAND (not carrying)");
        } else {
            // CARRYING: this re-pile is EITHER a churn re-pile (a re-grab follows within K -> CancelSettle) OR
            // the real land (no re-grab -> TickCarry COMMITs it). Hold the broadcast + ctx bump; capture the
            // payload. A later ToPile before commit just refreshes the settle (latest wins).
            g_carry[eid] = g_tick;
            LandSettle ls{};
            ls.countdown = kLandSettleTicks; ls.sincePile = 0;
            ls.chipType  = chipType; ls.loc = loc; ls.rot = rot; ls.cls = cls; ls.scale = scale;
            // take-31: remember the pile actor (+ its index) so the COMMIT can re-read its REAL, settled
            // transform (newActor is unpositioned NOW; loc/rot/scale above are the clump fallback).
            ls.pileActor = newActor; ls.pileIdx = R::InternalIndexOf(newActor);
            g_settle[eid] = ls;
            UE_LOGI("[TRASH-CH] HOST land-settle START eid=%u K=%d -- holding the ToPile (re-grab within K = "
                    "churn; no re-grab = the real land)", eid, kLandSettleTicks);
        }
    }
}

void OnHostRegrab(coop::element::ElementId E, void* newClump) {
    if (E == 0u || E == coop::element::kInvalidId || !newClump) return;
    const uint32_t eid = static_cast<uint32_t>(E);
    if (g_carry.find(eid) == g_carry.end()) return;       // not carrying -> not a churn re-grab of E
    g_carry[eid] = g_tick;
    RebindE(E, newClump);                                 // keep E on the live held clump (the carry stream continues)
    CancelSettle(eid, "re-grab");                         // a re-grab proves the preceding re-pile was churn
    UE_LOGI("[TRASH-CH] HOST carry re-grab eid=%u -- rebound onto the new held clump, settle cancelled "
            "(churn, not the land)", eid);
}

bool IsCarrying(coop::element::ElementId E) {
    return g_carry.find(static_cast<uint32_t>(E)) != g_carry.end();
}

bool HasPendingSettle(coop::element::ElementId E) {
    return g_settle.find(static_cast<uint32_t>(E)) != g_settle.end();
}

coop::element::ElementId AnyCarryingEid() {
    return g_carry.empty() ? coop::element::kInvalidId
                           : static_cast<coop::element::ElementId>(g_carry.begin()->first);
}

// (RULE 2, take-29 #2b: OnHostRelease -- the throw-edge ctx bump -- is RETIRED. The trash throw no longer
// sends a PropRelease (host-auth flight-stream + ToPile own the throw end); the LAND COMMIT's BroadcastConvert
// already bumps the ctx, so a separate throw bump is obsolete. Removed with its caller in local_streams.)

void NotePendingGrab(coop::element::ElementId pileEid, uint8_t chipType) {
    if (pileEid == 0u || pileEid == coop::element::kInvalidId) return;
    g_pendingGrab = PendingGrab{true, static_cast<uint32_t>(pileEid), chipType, kPendingGrabTtlTicks};
    UE_LOGI("[PILE] HOST grab PENDING eid=%u variant=%u -- the clump the BP spawns next adopts this eid "
            "(InpActEvt PRE seam; the BeginDeferred spawn is EX_CallMath -> invisible)",
            static_cast<unsigned>(pileEid), static_cast<unsigned>(chipType));
}

coop::element::ElementId AdoptPendingGrabClump(coop::net::Session& s, void* heldClump,
                                               const ue_wrap::FVector& clumpLoc,
                                               const ue_wrap::FRotator& clumpRot) {
    if (!g_pendingGrab.active || !heldClump) return coop::element::kInvalidId;
    const coop::element::ElementId E = static_cast<coop::element::ElementId>(g_pendingGrab.eid);
    const uint8_t chipType = g_pendingGrab.chipType;
    g_pendingGrab.active = false;                          // consume (one clump per grab)
    UE_LOGI("[PILE] HOST GRAB ADOPT eid=%u -- clump %p entered the hand -> binding it to the grabbed pile's "
            "eid + broadcasting convert", static_cast<unsigned>(E), heldClump);
    OnHostConvert(s, E, coop::net::propconvert_kind::kToClump, heldClump, clumpLoc, clumpRot, chipType);
    return E;
}

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

void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // GATE 1 (door CanOpen analog): already carrying -> mid-hold, deny. The host's PropConvert stream
    // already conveys the true state to the requester; no correction packet needed.
    if (g_carry.find(eid) != g_carry.end()) {
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
    if (!pile || !ue_wrap::prop::IsChipPile(pile)) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- pile actor not live / not a chipPile", eid, senderSlot);
        return;
    }

    // EXECUTE the real grab on puppet-N. The PROBE-PROVEN pattern (votv-puppet-grab-feasibility-RE-2026-06-22):
    // pile->playerGrabbed(Player=puppet); HitResult left zeroed (not a gate). The pile self-destructs in the
    // call (K2_DestroyActor(self)) -- do NOT deref `pile` afterwards. playerGrabbed sets the puppet's
    // grabbing_actor SYNCHRONOUSLY (pickupObject has no tick gate, RE-confirmed), so we read it on return.
    UE_LOGI("[GRAB-INTENT] EXEC puppet=%p pile=%p eid=%u slot=%u", puppet, pile, eid, senderSlot);
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

void ClearClientCarry(uint32_t eid) {
    if (eid != 0 && eid == g_clientCarry) {
        g_clientCarry = 0;
        UE_LOGI("[THROW-INTENT] CLIENT carry CLEARED eid=%u (carried proxy retired -- host aborted the carry)", eid);
    }
    if (eid != 0 && eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // also drop a pending request for a vanished eid
}

void TickCarry(coop::net::Session& s) {
    ++g_tick;
    // 1. pending-grab TTL (folded from the retired TickPendingGrab).
    if (g_pendingGrab.active && --g_pendingGrab.ttl <= 0) {
        UE_LOGI("[PILE] HOST grab PENDING eid=%u expired (no clump appeared -- grab produced none / hands full)",
                static_cast<unsigned>(g_pendingGrab.eid));
        g_pendingGrab.active = false;
    }
    // 2. land-settles: count down; COMMIT (broadcast the held ToPile + CLOSE the latch) on timeout. A re-grab
    //    (OnHostRegrab) cancels a settle before it can commit -> only a re-pile with NO following re-grab
    //    survives K ticks here = the real land (drop/throw/force).
    for (auto it = g_settle.begin(); it != g_settle.end(); ) {
        LandSettle& ls = it->second;
        ++ls.sincePile;
        if (--ls.countdown <= 0) {
            const coop::element::ElementId E = static_cast<coop::element::ElementId>(it->first);
            // take-31 FIX: re-read the pile's REAL transform NOW (post-FinishSpawning, the pile is positioned).
            // The thunk-captured ls.loc/rot/scale are the CLUMP fallback (the pile was unpositioned = (0,0,0)
            // at the BeginDeferred POST). A committing settle (survived K ticks with no re-grab) is the real
            // land, so its pile is alive + settled; IsLiveByIndex guards the cross-tick cached pointer.
            ue_wrap::FVector  cloc   = ls.loc;
            ue_wrap::FRotator crot   = ls.rot;
            ue_wrap::FVector  cscale = ls.scale;
            const bool reread = (ls.pileActor && R::IsLiveByIndex(ls.pileActor, ls.pileIdx));
            if (reread) {
                cloc   = ue_wrap::engine::GetActorLocation(ls.pileActor);
                crot   = ue_wrap::engine::GetActorRotation(ls.pileActor);
                cscale = ue_wrap::engine::GetActorScale3D(ls.pileActor);
            }
            BroadcastConvert(s, E, coop::net::propconvert_kind::kToPile, cloc, crot, cscale, ls.chipType,
                             ls.cls, "LAND COMMIT");
            UE_LOGI("[TRASH-CH] HOST LAND COMMIT eid=%u -- no re-grab within K -> the real land; carry CLOSED "
                    "(transform %s)", static_cast<unsigned>(E),
                    reread ? "re-read from the settled pile" : "FALLBACK (pile not live -- clump transform)");
            g_carry.erase(it->first);                     // CLOSE the carry latch
            g_heldBy.erase(it->first);                    // v84: the land ends a client-grab hold (re-grabbable)
            it = g_settle.erase(it);
        } else {
            ++it;
        }
    }
}

void ForgetEid(coop::element::ElementId E) {
    const uint32_t eid = static_cast<uint32_t>(E);
    const size_t a = g_carry.erase(eid);
    const size_t b = g_settle.erase(eid);                 // erase BOTH (no short-circuit) so neither leaks
    if (a || b)
        UE_LOGI("[TRASH-CH] HOST ForgetEid eid=%u -- carry latch + settle dropped (entity retired/destroyed)",
                static_cast<unsigned>(eid));
}

uint8_t CtxForEid(coop::element::ElementId E) {
    auto it = g_ctx.find(static_cast<uint32_t>(E));
    return (it == g_ctx.end()) ? uint8_t{0} : it->second;
}

bool AdoptInboundConvertCtx(coop::element::ElementId E, uint8_t ctx) {
    if (ctx == 0) return true;                            // legacy/non-trash -> no enforcement
    auto it = g_ctx.find(static_cast<uint32_t>(E));
    if (it != g_ctx.end() && it->second != 0 && !AtLeast(ctx, it->second)) {
        UE_LOGI("[PILE] CLIENT DROP stale convert eid=%u ctx=%u < known=%u (out-of-order / duplicate)",
                E, static_cast<unsigned>(ctx), static_cast<unsigned>(it->second));
        return false;                                    // an older convert than we've already applied
    }
    g_ctx[static_cast<uint32_t>(E)] = ctx;               // adopt the host's authoritative generation
    return true;
}

bool IsInboundStreamCtxFresh(coop::element::ElementId E, uint8_t ctx, bool requireCurrentGen) {
    if (ctx == 0) return true;                            // legacy/non-trash keyed prop -> no enforcement
    auto it = g_ctx.find(static_cast<uint32_t>(E));
    // A trash carry/throw (ctx>=1) for an eid we have seen NO convert for yet: the ToClump convert (reliable
    // lane) has not landed, so E still renders as a PILE here. Applying the clump's carry pose would DRIVE
    // the pile to the carry point + fire its grab cue BEFORE the re-skin -- the 2026-06-21 double-grab-sound
    // + pre-convert pile-jump glitch (the unreliable pose beats the reliable convert). HOLD until the convert
    // arrives (reliable -> it will); then g_ctx[E] is set and poses apply to the clump.
    if (it == g_ctx.end() || it->second == 0) return false;
    // requireCurrentGen splits the gate by packet kind (2026-06-21 sound fix, client-log root-caused):
    //   * CARRY POSE (true): apply ONLY the CURRENT generation (ctx == known). A pose for generation N drives
    //     the gen-N rendering, which exists ONLY after convert N is adopted (known==N). A pose AHEAD of its
    //     convert (ctx>known) is HELD -- else it drives the still-PRE-convert OLD rendering (the pile-jump +
    //     the SECOND grab-cue: the convert then re-skins -> the actor swaps -> a fresh GRAB-IN re-fires the
    //     cue = the triple sound). The reliable convert always lands, so the held pose is superseded by the
    //     continuous 30-60 Hz stream -- carry just starts AT the convert, no animation loss.
    //   * RELEASE (false): apply if NOT STALE (ctx >= known, AtLeast). A throw legitimately LEADS the last
    //     convert (ctx=N+1 over the grab's known=N) -- the throw is NOT a re-skin (the clump stays a clump),
    //     so it applies immediately to the current rendering; only a throw delayed past a re-pile drops.
    return requireCurrentGen ? (ctx == it->second) : AtLeast(ctx, it->second);
}

void OnDisconnect() {
    g_ctx.clear();
    g_pendingGrab = PendingGrab{};
    g_carry.clear();    // CLOSE-B: drop all carry latches + settles (gross reset)
    g_settle.clear();
    g_heldBy.clear();   // v84: drop all client-grab HELD_BY records
    g_clientPendingGrab = 0;  // v85: drop the client carry-state toggle
    g_clientCarry       = 0;
    g_tick = 0;
}

}  // namespace coop::trash_channel
