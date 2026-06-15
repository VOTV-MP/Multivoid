// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One function: EnsureHeldItemBroadcast. net_pump's held-prop send calls it on
// the new-held edge. The collect itself (trashBitsPile_C::playerTryToCollect)
// is BP-internal (ProcessInternal) so it can't be observed; we instead act on
// the grabbing_actor the send already resolves -- a freshly-spawned, auto-
// grabbed Aprop_C with Key=None gets a stable Key + a PropSpawn here, then the
// existing pose stream mirrors it into the collector's hands.

#include "coop/trash_collect_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "coop/remote_prop_spawn.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <string>
#include <vector>

namespace coop::trash_collect_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;

// Death-watch set. The clump's morph-destroy is unobservable (see header), so the
// owner liveness-checks every clump it broadcast and broadcasts the despawn when the
// actor dies. GAME-THREAD ONLY: both the held-edge (EnsureHeldItemBroadcast) and the
// per-tick sweep (TickWatchReleasedClumps) run from net_pump::Tick on the game thread,
// so no lock is needed. Bounded -- clumps are transient (usually 0-1 active); the cap
// is a runaway backstop. [[project-bug-trash-chippile-uaf-crash]]
struct WatchedClump {
    void*    actor       = nullptr;
    int32_t  internalIdx = -1;     // captured while live -> IsLiveByIndex without deref
    uint32_t eid         = 0;      // PropDestroy identity (key=None)
    ue_wrap::FVector lastPos{};    // updated each tick while alive -> the landed-pile search anchor
};
std::vector<WatchedClump> g_watchedClumps;
constexpr size_t kMaxWatchedClumps = 32;

// Mirror-pile death-watch (v52). A converted/landed trash pile lives as the owner's pile + the
// receiver's mirror pile, sharing one cross-peer eid. Grabbing the shared pile morphs+destroys it
// LOCALLY on the grabber (BP-internal/unobservable), so whoever grabs it watches it die here and
// broadcasts PropDestroy(eid) -> the others drop their mirror (identity-exact re-grab destroy,
// replacing the retired InpActEvt_use grab-guess). GAME-THREAD ONLY (net_pump tick). Bounded.
struct WatchedPile {
    void*    actor       = nullptr;
    int32_t  internalIdx = -1;   // captured live -> IsLiveByIndex without deref
    uint32_t eid         = 0;    // PropDestroy identity (key=None)
    ue_wrap::FVector lastPos{};  // fixed spawn pos (a settled pile never moves) -> proximity gate
};
std::vector<WatchedPile> g_watchedPiles;
// Cap is a runaway backstop, not a working limit. Fork B HALF 1 (2026-06-10)
// enrolls EVERY snapshot-expressed host pile (plus every mirror + convert),
// and the map's seeded garbagePileSpawner places hundreds -> 1024 headroom.
// The per-tick cost is just IsLiveByIndex (O(1)) over the live set; piles are
// stationary so there's no per-tick UFunction call. Shedding the oldest on
// overflow silently breaks that pile's re-grab destroy, so the cap stays well
// above any realistic pile count and the shed WARNs.
// 2026-06-13: bumped 1024 -> 2048 for the host EAGER enroll (prop_snapshot::
// StartEnumerationFor enrolls EVERY live chipPile up front -- ~870 on a heavy
// save -- so the cap needs clear headroom over the full pile count, not just
// "hundreds"). Per-tick cost stays O(n) IsLiveByIndex (no UFunction dispatch).
constexpr size_t kMaxWatchedPiles = 2048;
// Proximity radius (cm) for the grab-vs-stream-out discriminator: a grab happens AT the local
// player; a sublevel stream-out is far. 800 cm mirrors the grime super-sponge death-watch (which
// smoke-proved 0 false-fire on the connect stream-out). Erring loose: a miss silently breaks one
// re-grab destroy; a false fire (rare here) is a spurious PropDestroy a receiver no-ops on.
constexpr float kPileProximityCm  = 800.0f;
constexpr float kPileProximityCm2 = kPileProximityCm * kPileProximityCm;

// Register a freshly-broadcast clump for death-watching. Idempotent per eid.
void WatchClump(void* clump, uint32_t eid) {
    if (!clump || eid == 0) return;
    const int32_t idx = R::InternalIndexOf(clump);
    if (idx < 0) return;
    const ue_wrap::FVector pos = ue_wrap::engine::GetActorLocation(clump);
    for (auto& w : g_watchedClumps) {
        if (w.eid == eid) { w.actor = clump; w.internalIdx = idx; w.lastPos = pos; return; }
    }
    if (g_watchedClumps.size() >= kMaxWatchedClumps) {
        // Backstop: shed the oldest (should never trigger in normal play).
        g_watchedClumps.erase(g_watchedClumps.begin());
    }
    g_watchedClumps.push_back(WatchedClump{clump, idx, eid, pos});
}

// When a watched clump dies it CONVERTED: its hit-handler spawned a fresh chipPile at ~the clump's
// last position then self-destructed (the morph is BP-internal/unobservable, so the death-watch is
// how we learn of it). Find that pile -- an owner-side spatial query of OUR OWN just-spawned pile,
// which IS sound (the unsound case the DECISIVE RE retired was FindNearestChipPile on the RECEIVER,
// where piles are NOT co-located cross-peer; here the owner's pile is genuinely at pos). Mint a NEW
// eid for it, bind it locally, and broadcast ONE atomic PropConvert{oldEid=the dying clump's ball
// eid, newEid=the pile, transform, chipType, vel}. The receiver atomically destroys the ball mirror
// by oldEid AND spawns the pile by newEid -> no lingering ball, no double pile, no cross-peer
// position guess. Also enrols the owner's pile in the mirror-pile death-watch so a later re-grab of
// it propagates by identity. Returns true iff a pile was found + the convert broadcast (false = the
// clump expired WITHOUT converting -> caller falls back to a bare PropDestroy(oldEid) so the peer
// still despawns the flying mirror). RE: votv-clump-lifecycle-observability-...-pass2.md.
bool BroadcastConvertNear(uint32_t oldEid, const ue_wrap::FVector& pos, coop::net::Session* s) {
    float dist = -1.f;
    void* pile = ue_wrap::prop::FindNearestChipPile(pos, 200.f, &dist);
    if (!pile) return false;
    const std::wstring cls = R::ClassNameOf(pile);
    // Mint the authoritative pile eid + bind it to the owner's pile (so the owner's OWN later
    // re-grab of this pile resolves the eid for the mirror-pile death-watch). Idempotent.
    PT::MarkProcessedInit(pile);
    PT::MarkPropElement(pile, L"", cls);
    const coop::element::ElementId newEid = PT::GetPropElementIdForActor(pile);
    if (newEid == coop::element::kInvalidId) return false;
    coop::net::PropConvertPayload p{};
    p.oldEid = oldEid;
    p.newEid = static_cast<uint32_t>(newEid);
    p.pileClass.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.pileClass.data[p.pileClass.len++] = static_cast<char>(cls[i]);
    const ue_wrap::FVector  loc = ue_wrap::engine::GetActorLocation(pile);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(pile);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.chipType = ue_wrap::prop::GetChipType(pile);
    UE_LOGI("trash_collect: CONVERT ball eid=%u -> pile eid=%u cls='%ls' at (%.1f,%.1f,%.1f) "
            "variant=%u dist=%.1f",
            oldEid, p.newEid, cls.c_str(), p.locX, p.locY, p.locZ,
            static_cast<unsigned>(p.chipType), dist);
    s->SendReliable(coop::net::ReliableKind::PropConvert, &p, sizeof(p));
    // Fork B 2c: a convert-born pile announced while a snapshot bracket is
    // open is wire-expressed by US -- claim it or the adoption sweep at
    // SnapshotComplete destroys our own freshly-announced pile. Also keeps
    // the watched-piles-are-always-claimed invariant for owner-side watches.
    coop::remote_prop_spawn::RecordClaimIfTracking(pile);
    // Watch the owner's own pile so the owner's re-grab of it (the pile morph-destroys locally)
    // broadcasts PropDestroy(newEid) -> the receiver drops its mirror pile.
    WatchPile(pile, p.newEid);
    return true;
}

}  // namespace

bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* s) {
    if (!heldActor || !s || !s->connected()) return false;
    if (!R::IsLive(heldActor)) return false;
    // Any KEYED interactable: Aprop_C trash items AND the non-Aprop_C
    // garbageClump/chipPile (the actual trash the player carries). CRASH-SAFETY
    // is enforced on the RECEIVER, not by excluding them here: GetStaticMesh
    // returns null for non-Aprop_C, so the peer never runs physics on the clump
    // mirror -- it spawns physics-free and is driven KINEMATICALLY (the inverse
    // of the reverted-2a UAF). [[project-bug-trash-chippile-uaf-crash]]
    if (!ue_wrap::prop::IsKeyedInteractable(heldActor)) return false;

    // A divergence sweep is pending and this held actor is one of its candidates
    // (an in-universe, unclaimed save-loaded local the host did NOT express -- a
    // ghost awaiting adjudication, e.g. the save-transfer kerfur the host turned
    // ON before this client joined). Do NOT express it: a SendPropSpawn below
    // would fresh-spawn a host duplicate, and the RecordClaimIfTracking after it
    // would claim the ghost past the pending sweep (permanently rescuing it). Let
    // the deferred sweep destroy it. A genuinely client-originated drop is claimed
    // via the takeObj path, so it is NOT a candidate and streams normally here.
    if (coop::remote_prop_spawn::IsPendingSweepCandidate(heldActor)) {
        UE_LOGI("trash_collect: held item %p is a pending divergence-sweep candidate "
                "(unclaimed ghost) -- NOT expressing (the deferred sweep will destroy it)", heldActor);
        return false;
    }

    // Express-if-unknown (ghost-twin fix, 2026-06-10 hands-on forensics). The
    // old predicate skipped ANY keyed held actor as "a normal world prop the
    // peer already has" -- but "has a key" is NOT "peer has it": a prop that
    // materialized in the world-load's late tail (keyless at sweep/seed time,
    // key self-minted by its Init AFTER both) is keyed yet completely unknown
    // to the wire -- never expressed, never bound, eid=0. The user carrying
    // one was invisible to the host (127 'no local match' PropPose warns) and
    // the wall stack diverged one-way. The correct skip test is TRACKER-KNOWN:
    // a prop with a live Element (seed-walked / Init-announced / previously
    // expressed) is genuinely shared -- the pose stream alone mirrors it. An
    // untracked one gets expressed RIGHT HERE, key and all, the moment a
    // player first touches it -- the self-heal for any straggler class.
    // (The peer's OnSpawn may fuzzy-bind it to a nearby same-class prop; for a
    // genuine EXTRA actor that is bounded weirdness vs the strictly-worse
    // one-way invisibility. The structural fix for the late-tail stragglers
    // themselves is tracked separately -- see the sweep's keyless histogram.)
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(heldActor);
    if (!keyStr.empty() && keyStr != L"None" &&
        PT::GetPropElementIdForActor(heldActor) != coop::element::kInvalidId) {
        return false;  // keyed AND tracker-known: the peer has it; pose stream suffices
    }

    const std::wstring cls = R::ClassNameOf(heldActor);
    // Aprop_C trash items get a force-minted Key (their UCS holds it). The non-Aprop_C
    // trash CLUMP (prop_garbageClump_C) is NON-KEYABLE -- setKey doesn't stick (the
    // source re-reads None, autotest 33e7f25) -- so it rides the SAME prop pipeline but
    // is identified by our EID instead: broadcast with key=None + the eid, and the
    // receiver resolves its mirror by eid (PropPoseSnapshot.elementId, v26). The clump
    // renders on its own (bare spawn = the 'dirtball' mesh), so no mesh transfer needed.
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool isAprop = ue_wrap::prop::IsDescendantOfProp(heldActor);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(heldActor, keyStr, /*mintForAprop=*/isAprop);
    if (keyStr.empty() || keyStr == L"None") {
        if (isAprop) {
            UE_LOGW("trash_collect: Aprop item %p cls='%ls' Key still None after force-mint -- cannot mirror",
                    heldActor, cls.c_str());
            return false;
        }
        keyStr.clear();  // clump: wire key stays None; the eid is the cross-peer identity
    }
    // Create the Prop Element shadow + dedupe latch so the item's eventual
    // K2_DestroyActor unwinds it through the normal destroy path.
    PT::MarkProcessedInit(heldActor);
    PT::MarkPropElement(heldActor, keyStr, cls);

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i)
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);

    const ue_wrap::FVector  loc = ue_wrap::engine::GetActorLocation(heldActor);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(heldActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v54: real scale (was hardcoded 1,1,1) + identity row below.
    const ue_wrap::FVector scl = ue_wrap::engine::GetActorScale3D(heldActor);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // Carry the trash VARIANT so the mirror shows the same chip/clump type the owner
    // grabbed (else a bare spawn defaults to variant 0 = the wrong-type bug). 0 for any
    // non-trash actor (GetChipType is a reflection no-op without a chipType property).
    p.chipType = ue_wrap::prop::GetChipType(heldActor);
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    // IsHeavy/IsFrozen read Aprop_C struct offsets -- only meaningful on Aprop_C.
    // For a non-Aprop_C clump the receiver ignores physFlags (kinematic mirror),
    // so don't read stray bytes off it.
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        if (ue_wrap::prop::IsHeavy(heldActor))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(heldActor)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(heldActor)) p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(heldActor)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(heldActor)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        // v54 identity: the list_props row -- a held GENERIC prop_C (e.g. a
        // cubicle gib panel) must mirror as itself, not the CDO 'cube'.
        const std::wstring nm = ue_wrap::prop::GetPropNameString(heldActor);
        for (size_t i = 0; i < nm.size() && i < 31; ++i) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[i]);
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(heldActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    UE_LOGI("trash_collect: BROADCAST held untracked item cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) "
            "-- held-pose stream now mirrors it into the collector's hands",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ);
    s->SendPropSpawn(p);
    // Fork B 2c: self-claim -- this peer just wire-expressed the held item;
    // an open bracket's sweep must not destroy it as "unclaimed".
    coop::remote_prop_spawn::RecordClaimIfTracking(heldActor);
    // Non-keyable trash CLUMP (key was cleared above): its eventual morph-destroy is
    // UNOBSERVABLE (bypasses the K2_DestroyActor observer), so death-watch it -- the
    // tick its actor dies we broadcast the despawn by eid. Keyed Aprop_C trash items
    // are NOT watched: their destroy fires the observer normally (the keyed path works).
    if (keyStr.empty() && p.elementId != 0) {
        WatchClump(heldActor, p.elementId);
    }
    return true;
}

void TickWatchReleasedClumps(coop::net::Session* s) {
    if (!s || !s->connected() || g_watchedClumps.empty()) return;
    for (size_t i = 0; i < g_watchedClumps.size();) {
        WatchedClump& w = g_watchedClumps[i];
        if (R::IsLiveByIndex(w.actor, w.internalIdx)) {
            w.lastPos = ue_wrap::engine::GetActorLocation(w.actor);  // track for the landed-pile search
            ++i;
            continue;
        }
        // The clump's actor went dead -> it CONVERTED (re-piled on landing) or its LifeSpan
        // expired. Emit ONE atomic PropConvert if it re-piled: the receiver destroys the ball
        // mirror by oldEid AND spawns the pile by newEid in a single handler (no separate
        // spawn+destroy race, no lingering ball, no double pile). If it expired WITHOUT
        // converting (no pile near lastPos), fall back to a bare PropDestroy(oldEid) so the peer
        // still despawns the flying mirror. RE: ...-robust-design-...-pass2.md.
        const bool converted = BroadcastConvertNear(w.eid, w.lastPos, s);
        if (!converted) {
            coop::net::PropDestroyPayload dp{};
            dp.key.len   = 0;          // key=None: the clump is eid-only
            dp.elementId = w.eid;
            s->SendPropDestroy(dp);
            UE_LOGI("trash_collect: watched clump eid=%u died WITHOUT converting -> PropDestroy (despawn mirror)",
                    w.eid);
        }
        g_watchedClumps.erase(g_watchedClumps.begin() + i);
    }
}

// ---- mirror-pile death-watch (v52) -----------------------------------------------------

void WatchPile(void* pileActor, uint32_t eid, bool quiet) {
    if (!pileActor || eid == 0 || eid == coop::element::kInvalidId) return;
    WatchPileAt(pileActor, eid, ue_wrap::engine::GetActorLocation(pileActor), quiet);
}

void WatchPileAt(void* pileActor, uint32_t eid, const ue_wrap::FVector& pos, bool quiet) {
    // Perf audit W-3 (2026-06-10): the snapshot drain already read the pile's
    // location for the payload -- this overload skips the duplicate
    // GetActorLocation ProcessEvent dispatch per expressed pile.
    if (!pileActor || eid == 0 || eid == coop::element::kInvalidId) return;
    const int32_t idx = R::InternalIndexOf(pileActor);
    if (idx < 0) return;
    for (auto& w : g_watchedPiles) {
        if (w.eid == eid) { w.actor = pileActor; w.internalIdx = idx; w.lastPos = pos; return; }
    }
    if (g_watchedPiles.size() >= kMaxWatchedPiles) {
        // Backstop: shed oldest. A shed entry's re-grab destroy silently
        // breaks -- WARN so a map exceeding the cap is visible in the log.
        UE_LOGW("trash_collect: watched-pile cap %zu hit -- shedding eid=%u (its re-grab destroy is lost; raise kMaxWatchedPiles)",
                kMaxWatchedPiles, g_watchedPiles.front().eid);
        g_watchedPiles.erase(g_watchedPiles.begin());
    }
    g_watchedPiles.push_back(WatchedPile{pileActor, idx, eid, pos});
    if (!quiet)
        UE_LOGI("trash_collect: watching pile eid=%u actor=%p at (%.1f,%.1f,%.1f) for re-grab destroy",
                eid, pileActor, pos.X, pos.Y, pos.Z);
}

void NotifyPileConsumed(uint32_t eid) {
    for (size_t i = 0; i < g_watchedPiles.size(); ++i) {
        if (g_watchedPiles[i].eid == eid) {
            g_watchedPiles.erase(g_watchedPiles.begin() + i);
            UE_LOGI("trash_collect: pile eid=%u consumed by incoming wire destroy -> dropped watch "
                    "(no re-broadcast)", eid);
            return;
        }
    }
}

void TickWatchReleasedPiles(coop::net::Session* s, bool suppress) {
    if (!s || !s->connected() || g_watchedPiles.empty()) return;
    // Read the local camera ONCE, only when at least one pile died this tick (the common case is
    // none). A pile grab happens AT the local player; a far death is a sublevel stream-out.
    bool camValid = false;
    ue_wrap::FVector cam{};
    for (size_t i = 0; i < g_watchedPiles.size();) {
        WatchedPile& w = g_watchedPiles[i];
        if (R::IsLiveByIndex(w.actor, w.internalIdx)) {
            // NO per-tick GetActorLocation here: a landed chipPile is SETTLED (placed, not
            // simulating) -> it never moves, so the spawn-time lastPos captured in WatchPile stays
            // accurate for the proximity gate. (The clump watch above legitimately re-reads each
            // tick because clumps are airborne; piles are not.) Audit perf-WARN 2026-06-09.
            ++i;
            continue;
        }
        // Dead. Broadcast PropDestroy ONLY for a NEAR death outside a world-transition window (a
        // grab); far / transition deaths are stream-outs -> drop the entry silently (it re-watches
        // on a future convert). The grime super-sponge precedent (0 false-fire on connect stream-out).
        bool grabbed = false;
        if (!suppress) {
            if (!camValid) { cam = ue_wrap::engine::GetCameraLocation(); camValid = true; }
            const float dx = cam.X - w.lastPos.X, dy = cam.Y - w.lastPos.Y, dz = cam.Z - w.lastPos.Z;
            grabbed = (dx * dx + dy * dy + dz * dz) <= kPileProximityCm2;
        }
        if (grabbed) {
            coop::net::PropDestroyPayload dp{};
            dp.key.len   = 0;          // key=None: the pile is eid-only
            dp.elementId = w.eid;
            s->SendPropDestroy(dp);
            UE_LOGI("trash_collect: watched pile eid=%u grabbed locally (died near camera) -> "
                    "PropDestroy (drop peer mirror)", w.eid);
        } else {
            UE_LOGI("trash_collect: watched pile eid=%u died far/transition (stream-out) -> drop watch silently",
                    w.eid);
        }
        g_watchedPiles.erase(g_watchedPiles.begin() + i);
    }
}

void OnDisconnect() {
    g_watchedClumps.clear();
    g_watchedPiles.clear();
}

}  // namespace coop::trash_collect_sync
