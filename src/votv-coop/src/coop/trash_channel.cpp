// coop/trash_channel.cpp -- see coop/trash_channel.h.

#include "coop/trash_channel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/remote_prop.h"   // RegisterPropMirror (the single rebind entry point)
#include "ue_wrap/engine.h"      // GetActorScale3D (v83 per-form proxy scale)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"  // ClassNameOf
#include "ue_wrap/types.h"       // FVector / FRotator / NormalizeAxis

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
    s.SendReliable(coop::net::ReliableKind::PropConvert, &p, sizeof(p));
    UE_LOGI("[TRASH-CH] HOST BROADCAST %s eid=%u ctx=%u (%s) at (%.1f,%.1f,%.1f) variant=%u",
            kind == coop::net::propconvert_kind::kToClump ? "ToClump" : "ToPile",
            static_cast<unsigned>(E), static_cast<unsigned>(ctx), why,
            p.locX, p.locY, p.locZ, static_cast<unsigned>(chipType));
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
    g_tick = 0;
}

}  // namespace coop::trash_channel
