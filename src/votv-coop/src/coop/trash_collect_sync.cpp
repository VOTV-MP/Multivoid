// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One function: EnsureHeldItemBroadcast. net_pump's held-prop send calls it on
// the new-held edge. The collect itself (trashBitsPile_C::playerTryToCollect)
// is BP-internal (ProcessInternal) so it can't be observed; we instead act on
// the grabbing_actor the send already resolves -- a freshly-spawned, auto-
// grabbed Aprop_C with Key=None gets a stable Key + a PropSpawn here, then the
// existing pose stream mirrors it into the collector's hands.

#include "coop/trash_collect_sync.h"

#include "coop/kerfur_entity.h"  // K-5: IsKerfurActor (the held-kerfur class-gate)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "coop/remote_prop.h"        // ResolveMirrorEidByActor (the pile-grab hook mirror eid resolve)
#include "coop/remote_prop_spawn.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"     // RegisterPreObserver (the InpActEvt_use pile-grab observer)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"     // MainPlayerClass + MainPlayerUseInputEventFn
#include "ue_wrap/types.h"

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace coop::trash_collect_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

// Cached Session for the pile-grab observer (the PRE callback can't take a param). Set by Install
// (re-cached every call for reconnect). Game-thread read inside the observer.
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_grabObserverInstalled = false;  // InpActEvt_use PRE registration latch (stays for process life)

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

// NOTE (2026-06-17, RULE 1+2): the mirror-PILE death-watch that used to live here (g_watchedPiles +
// WatchPile/WatchPileAt/NotifyPileConsumed/TickWatchReleasedPiles) is RETIRED. It inferred "grabbed"
// from "a watched pile's actor died NEAR the local camera", which is UNSOUND: a chipPile is a mobile,
// physics-simulating actor that dies near the camera for many non-grab reasons (a peer bumping it,
// a physics nudge through the floor, a LifeSpan/removeWOrespawn despawn). Any such near-camera death
// was misread as a grab -> a spurious PropDestroy(eid) -> the receiver's unconditional K2_DestroyActor
// wiped the pile on BOTH peers. Since BOTH peers watch the same pile, a peer repeatedly TOUCHING a pile
// eventually triggered a near-camera non-grab death and the pile vanished for everyone -- the recurring
// "piles destroyed when a peer touches them a lot" bug. The DECISIVE RE
// (votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md) proved the correct, strictly-better mechanism
// is OnPileGrabPre below: a PRE observer on the (ProcessEvent-visible) E-press InpActEvt_use that reads
// lookAtActor -> the pile, still ALIVE + eid-resolvable -> PropDestroy(eid). It fires ONLY on a real
// grab (a bump/stream-out/physics-death is not an E-press, so it can never enter that path). The CLUMP
// death-watch below stays -- a thrown clump's morph-to-pile conversion is genuinely unobservable and
// has no input edge, so the owner-side liveness poll is the only signal (and clumps are airborne &
// transient, not the stationary piles a player walks up to and bumps).

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
    // The owner's later re-grab of this converted pile is caught by OnPileGrabPre (the InpActEvt_use
    // PRE observer reads lookAtActor=pile -> newEid -> PropDestroy) -> the receiver drops its mirror.
    // (This used to enroll the pile in the now-retired pile death-watch; see the note above.)
    return true;
}

}  // namespace

bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* s) {
    if (!heldActor || !s || !s->connected()) return false;
    if (!R::IsLive(heldActor)) return false;
    // K-5 kerfur class-gate: a kerfur prop is a host-owned entity (the host owns its KerfurId + its
    // host-range eid). A client must NEVER express/mint a peer-range eid for it -- that was the
    // grab-dupe root (redesign Failures #1/#3/#6: the minted peer-range eid is dropped by the host's
    // host-range conversion gate, then re-mirrored as a fresh client entity -> the dupe-and-drop loop).
    // Never mint here; the held-pose stream (local_streams) instead carries the kerfur prop MIRROR by
    // its host-range eid (kerfur_entity::GetKerfurMirrorEidForActor), which the host's remote_prop
    // receiver resolves to the authoritative kerfur prop + kinematic-drives -- no PropSpawn needed. On
    // the HOST this is redundant (its kerfur prop is tracker-known -> the express skip below already
    // returns false), but gating up-front is role-agnostic + explicit.
    if (coop::kerfur_entity::IsKerfurActor(heldActor)) return false;
    // Any KEYED interactable: Aprop_C trash items AND the non-Aprop_C
    // garbageClump/chipPile (the actual trash the player carries). CRASH-SAFETY
    // is enforced on the RECEIVER, not by excluding them here: GetStaticMesh
    // returns null for non-Aprop_C, so the peer never runs physics on the clump
    // mirror -- it spawns physics-free and is driven KINEMATICALLY (the inverse
    // of the reverted-2a UAF). [[project-bug-trash-chippile-uaf-crash]]
    if (!ue_wrap::prop::IsKeyedInteractable(heldActor)) return false;
    // PART 2 (2026-06-18) CLIENT host-authority gate for SHARED TRASH (chipPile / garbageClump /
    // trashBitsPile -- the non-Aprop_C "world litter"). These are HOST-OWNED shared entities. When a
    // CLIENT grabs a host pile, its BP locally morphs it to a clump (EX_CallMath, un-hookable) and this
    // path would SendPropSpawn that clump -> the host fresh-spawns a DUPLICATE, and on the clump's later
    // land-convert (BroadcastConvertNear, reachable only if we WatchClump below) the host spawns yet
    // another pile = "a new pile born from the host's original, from the host's perspective." The client
    // must NEVER author a shared trash entity -- the exact host-authority rule the K-5 kerfur gate above
    // enforces. Suppressing here ALSO disarms the clump-convert watch (WatchClump is only reached past
    // this return), so neither the clump PropSpawn nor the PropConvert ever leaves the client -> the dupe
    // is killed at the SOURCE. The client's grab already removed the host's original via OnPileGrabPre's
    // PropDestroy (eid-resolved on the mirror), so the original IS removed -- no orphan. The client holds
    // the clump locally; a client-THROWN ground pile staying client-local is a known phase-2 gap (the
    // host-request spawn model) -- a benign divergence, NOT a dupe. Aprop_C items (IsDescendantOfProp)
    // are unaffected (per-player carry stays). The HOST still authors its own held trash below.
    // (Audit M1, 2026-06-18: this suppresses all 3 shared-trash bases but OnPileGrabPre's host-original
    // PropDestroy covers only chipPile -- safe because the other two are never carried-as-a-host-mirror:
    // a trashBitsPile dispenses an Aprop item + a separately-synced counter (not a held clump), and a
    // garbageClump only ever exists as a chipPile's morph product, whose chipPile grab already fired
    // OnPileGrabPre. So no host original is ever left orphaned.)
    if (s->role() == coop::net::Role::Client && !ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        UE_LOGI("trash_collect: CLIENT holds shared trash cls='%ls' -- NOT authoring it (host owns world "
                "piles/clumps; the grab's PropDestroy already removed the host original). No dupe.",
                R::ClassNameOf(heldActor).c_str());
        return false;
    }

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

    // Pre-quiescence JOIN WINDOW (2026-06-16 hands-on, the "kerfur off+grab dupe"). The guard above
    // only fires once the divergence sweep is ARMED (g_sweepPending). But a save-loaded in-universe
    // keyed prop the client grabs in the ~25 s window BEFORE the snapshot bracket opens slips past it
    // (g_sweepPending still false) -- and expressing it here SendPropSpawns a host duplicate the
    // client-side sweep can NEVER reclaim (it ran: host fresh-spawned a kerfur prop, eid 43938,
    // ownerSlot=1, permanent). Same divergence-universe membership, just not yet sweep-armed: until
    // load-tail quiescence the world is NOT reconciled, so a grabbed un-adjudicated local is not ours
    // to announce. Keep holding it locally (no pop); the host expresses its OWN copy in the bracket
    // (our held copy is then key/fuzzy-matched + claimed -> the held-pose stream mirrors it, no dupe),
    // or the sweep destroys our copy if the host converted it away. HasLoadTailQuiesced flips true the
    // instant the sweep fires, so this gates ONLY the join window -- never steady-state gameplay.
    if (!coop::remote_prop_spawn::HasLoadTailQuiesced() &&
        coop::remote_prop_spawn::IsInDivergenceUniverseUnclaimed(heldActor)) {
        UE_LOGI("trash_collect: held item %p cls='%ls' grabbed PRE-QUIESCENCE (join window not yet "
                "reconciled) -- NOT expressing (host expresses its own / the sweep adjudicates ours; "
                "prevents the off+grab host dupe)", heldActor, R::ClassNameOf(heldActor).c_str());
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

// ---- pile-grab destroy: the InpActEvt_use PRE observer (RULE-1 replacement for the death-watch) ----
//
// DECISIVE RE (votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md, summary table): a chipPile grab
// is the E-press input action AmainPlayer_C::InpActEvt_use_..._41 -> icast(lookAtActor)->playerGrabbed
// (spawn clump + pickupObjectDirect(clump) + K2_DestroyActor(self)), all dispatched BP-internally
// (EX_LocalVirtualFunction / EX_VirtualFunction -> ProcessInternal) and INVISIBLE to our ProcessEvent
// detour. The ONLY ProcessEvent-observable edge with the pile STILL ALIVE + eid-resolvable is a PRE
// observer on InpActEvt_use (the native input -> UFunction dispatch DOES hit ProcessEvent -- the same
// seam door + kerfur-menu sync use), reading mainPlayer.lookAtActor = the pile under the crosshair
// BEFORE the ubergraph converts+destroys it. This fires ONLY on a real grab: a bump, a stream-out, a
// physics death, a LifeSpan despawn are NOT an E-press, so they can never enter this path -- which is
// exactly why it replaces the unsound proximity death-watch (the recurring "piles destroyed when a
// peer touches them" bug). Symmetric (BOTH roles): the owner re-grabbing its own pile resolves via the
// forward map (GetPropElementIdForActor); a peer grabbing a host-owned mirror pile resolves via
// remote_prop::ResolveMirrorEidByActor. Puppets are unpossessed -> never process input -> this only
// ever fires for the local player.
static void OnPileGrabPre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;  // BOTH roles -- whoever physically grabs
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the pile (PRE-conversion, alive)
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return;         // E-press not aimed at a pile
    // Cross-peer identity: a pile WE own is in the forward map; a pile WE mirror is in the prop
    // MirrorManager. Either resolves THE shared eid the other peer keys its copy on.
    coop::element::ElementId eid = PT::GetPropElementIdForActor(aimed);
    if (eid == coop::element::kInvalidId)
        eid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    if (eid == coop::element::kInvalidId || eid == 0u) return;  // untracked pile -> no peer mirror to drop
    // InpActEvt_use dispatches on BOTH the press AND the release of one tap; the press already
    // converts+destroys the pile so the release is a natural no-op (lookAtActor no longer the pile),
    // but collapse a same-eid repeat within a tap window defensively. One E-press grabs one pile, so a
    // single (eid,time) slot suffices -- no unbounded per-pile map.
    static coop::element::ElementId s_lastEid = coop::element::kInvalidId;
    static std::chrono::steady_clock::time_point s_lastTs{};
    const auto now = std::chrono::steady_clock::now();
    if (eid == s_lastEid && now - s_lastTs < std::chrono::milliseconds(300)) return;
    s_lastEid = eid;
    s_lastTs  = now;
    coop::net::PropDestroyPayload dp{};
    dp.key.len   = 0;     // chipPile is eid-only (Key=None on the wire)
    dp.elementId = static_cast<uint32_t>(eid);
    s->SendPropDestroy(dp);
    UE_LOGI("trash_collect: pile grab (E-press on chipPile eid=%u) -> PropDestroy (peer drops its mirror)",
            static_cast<unsigned>(eid));
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    if (g_grabObserverInstalled) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;  // mainPlayer_C not loaded yet -> retry on the next world-gated Install
    void* fn = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("trash_collect: InpActEvt_use UFunction not found -- pile grabs cannot drop peer mirrors");
        g_grabObserverInstalled = true;  // permanent give-up (don't re-walk the class forever)
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnPileGrabPre)) {
        UE_LOGW("trash_collect: InpActEvt_use PRE observer register failed (table full?)");
        return;  // not latched -> retry next Install
    }
    g_grabObserverInstalled = true;
    UE_LOGI("trash_collect: pile-grab observer installed on InpActEvt_use (E-press on a chipPile -> PropDestroy(eid))");
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_watchedClumps.clear();
}

}  // namespace coop::trash_collect_sync
