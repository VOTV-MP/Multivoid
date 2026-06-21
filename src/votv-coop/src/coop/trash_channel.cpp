// coop/trash_channel.cpp -- see coop/trash_channel.h.

#include "coop/trash_channel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/remote_prop.h"   // RegisterPropMirror (the single rebind entry point)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"  // ClassNameOf
#include "ue_wrap/types.h"       // FVector / FRotator / NormalizeAxis

#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::trash_channel {
namespace {

namespace R = ue_wrap::reflection;

// Per-eid sync-time-context. HOST: the AUTHORITY (BumpCtx writes via OnHostConvert/OnHostRelease).
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

}  // namespace

void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType) {
    if (E == 0u || E == coop::element::kInvalidId || !newActor) return;
    const uint8_t ctx = Bump(static_cast<uint32_t>(E));   // BUMP on every transition (host-authoritative)

    RebindE(E, newActor);                                 // re-point E onto the new rendering locally

    coop::net::PropConvertPayload p{};
    p.oldEid = static_cast<uint32_t>(E);                  // bind model: oldEid == newEid == E
    p.newEid = static_cast<uint32_t>(E);
    const std::wstring cls = R::ClassNameOf(newActor);
    p.pileClass.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.pileClass.data[p.pileClass.len++] = static_cast<char>(cls[i]);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.chipType = chipType;
    p.kind     = kind;
    p.ctx      = ctx;
    s.SendReliable(coop::net::ReliableKind::PropConvert, &p, sizeof(p));
    UE_LOGI("[PILE] HOST %s eid=%u ctx=%u cls='%ls' at (%.1f,%.1f,%.1f) variant=%u -> rebound E locally + "
            "broadcast PropConvert (clients re-skin their ONE mirror of E -- zero-proximity spawn-POST link)",
            kind == coop::net::propconvert_kind::kToClump ? "GRAB(pile->clump)" : "LAND(clump->pile)",
            E, static_cast<unsigned>(ctx), cls.c_str(), p.locX, p.locY, p.locZ,
            static_cast<unsigned>(chipType));
}

uint8_t OnHostRelease(coop::element::ElementId E) {
    auto it = g_ctx.find(static_cast<uint32_t>(E));
    if (it == g_ctx.end()) return 0;                      // not a tracked trash entity -> no ctx
    const uint8_t ctx = Bump(static_cast<uint32_t>(E));   // the THROW edge is logged by local_streams (it has |v|)
    return ctx;
}

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

void TickPendingGrab() {
    if (g_pendingGrab.active && --g_pendingGrab.ttl <= 0) {
        UE_LOGI("[PILE] HOST grab PENDING eid=%u expired (no clump appeared -- grab produced none / hands full)",
                static_cast<unsigned>(g_pendingGrab.eid));
        g_pendingGrab.active = false;
    }
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
}

}  // namespace coop::trash_channel
