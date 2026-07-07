// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One function: EnsureHeldItemBroadcast. net_pump's held-prop send calls it on
// the new-held edge. The collect itself (trashBitsPile_C::playerTryToCollect)
// is BP-internal (ProcessInternal) so it can't be observed; we instead act on
// the grabbing_actor the send already resolves -- a freshly-spawned, auto-
// grabbed Aprop_C with Key=None gets a stable Key + a PropSpawn here, then the
// existing pose stream mirrors it into the collector's hands.

#include "coop/props/trash_collect_sync.h"

#include "coop/dev/spawn_order_probe.h"  // Phase 1 step 1A: client load-spawn coverage probe (read-only)
#include "coop/props/save_identity_bind.h"     // Phase 1 step 2b: client eid-range bind (mutating)
#include "coop/props/save_identity_map.h"      // Family
#include "coop/creatures/kerfur_entity.h"  // K-5: IsKerfurActor (the held-kerfur class-gate); IsKerfurPropClass (off-prop)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"       // DestroyLocalProp (v106: post-quiescence unbound ghost retire)
#include "coop/props/prop_sound.h"           // client-own-grab pickup cue (native grab suppressed -> synthesize locally)
#include "coop/props/prop_synth_key.h"
#include "coop/props/remote_prop.h"        // ResolveMirrorEidByActor (the pile-grab hook mirror eid resolve)
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/session/save_transfer.h"      // docs/piles/09: RecordGrabTimePileXform (grab-edge save-time key)
#include "coop/props/trash_channel.h"      // NoteClumpBorn (v106: the clump birth certificate; docs/piles/08)
#include "coop/props/trash_proxy.h"        // EidForAimedPileProxy (Increment 2: client-grab camera-ray cone recognition)
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"     // RegisterPreObserver (the InpActEvt_use pile-grab observer)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"     // MainPlayerClass + MainPlayerUseInputEventFn
#include "ue_wrap/types.h"
#include "ue_wrap/ufunction_hook.h"  // the deterministic re-pile: BeginDeferred Func-patch (docs/piles/08)

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

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
bool g_dropGrabThunkInstalled = false;  // read-only dropGrabObject Func-thunk latch (the PHC-grab release seam)

// GESTURE-PAIRING latch (2026-07-02, the "EHHH on E-drop" root): when a CLIENT use-PRESS dispatch is
// CANCELLED (any press seam), its paired IE_Released dispatch (_42) must be cancelled too -- the BP never
// saw the press, so its release handler runs against a press-less state and hits useAction's use_deny.
// Re-deriving the press conditions AT RELEASE TIME is provably wrong in both directions: (a) the throw
// press optimistically clears g_clientCarry (trash_channel::SendThrowIntent), so "carrying?" reads false
// ~1 frame later at the release -> the deny leaked through (the reported bug); (b) a release paired with
// a NATIVE press (e.g. dropping a natively-held prop while HAPPENING to aim at a pile) matched the aim
// conditions and was wrongly eaten. Pairing is deterministic: armed by every cancelled press seam,
// consumed by the next _42 fire. UE guarantees the release arrives (FlushPressedKeys dispatches
// IE_Released on focus loss/unpossess), so the latch cannot go stale. Game-thread only (input dispatch).
bool g_cancelPairedUseRelease = false;

// RULE 1+2 (2026-06-21, docs/piles/08): the v81 MORPH (proximity FindNearestChipPile land-detect) is FULLY
// RETIRED -- it mis-bound on a NEIGHBOUR pile in a dense cluster. The host-grab sync is now: the InpActEvt
// clump's identity travels via the v106 BIRTH CERTIFICATE (trash_channel::NoteClumpBorn at the BeginDeferred
// thunk); the held-edge adopts the clump onto that eid; identity is the host eid end-to-end (POSITION IS NEVER IDENTITY).
// (An earlier design caught the spawn at a host_spawn_watcher BeginDeferred POST -- RETIRED 2026-06-21: the
// chipPile/clump spawn is dispatched EX_CallMath, INVISIBLE to our ProcessEvent hook; it never fired.)

// (The proximity RE-PILE death-watch -- WatchClumpForRepile / Tick / FindNearestUntrackedChipPile_ /
// g_watchedClumps -- is RETIRED 2026-06-21, RULE 2. It converted on the clump's DEATH by a nearest-untracked
// pile search; the deterministic UFunction::Func thunk (OnBeginDeferredSpawnObserve) replaced it after a
// hands-on validated the thunk's *Result is ptr-for-ptr the same pile the death-watch found. The thunk
// converts the EXACT spawned pile the same tick -- no proximity, no reaper race. A consumed clump (no
// re-pile spawn) now just dies untracked -> the normal prop reaper PropDestroys it.)

bool g_repileThunkInstalled = false;          // process-lifetime Func-patch latch

// THE DETERMINISTIC RE-PILE (docs/piles/08 "thunk") -- the convert, validated GREEN 2026-06-21 (host log:
// many CLEAN [REPILE], the thunk's *Result ptr-for-ptr == the death-watch's FindNearest pile every isolated
// re-pile). On a host re-pile the clump's `EX_CallMath BeginDeferredActorSpawnFromClass(self=clump, pile)`
// fires our UFunction::Func patch -> (srcObj = FFrame::Object = the re-piling clump @0x18; newActor =
// *Result = the new chipPile). If the clump is a TRACKED trash entity (eid E) we convert E onto the new pile
// IN PLACE, the SAME tick the pile is constructed -- zero proximity, zero death-watch, no reaper race (so the
// old ~5s vanish-return is gone by construction). The GRAB case (srcObj=pile, newActor=clump) is NOT a clump
// source -> ignored here; the host grab stays on the InpActEvt PRE + held-edge adopt path. A wild srcObj (a
// wrong offset -- ruled out, IDA-pinned + validated) would fault in IsGarbageClump and the facility's
// RunCbSEH absorbs it -> no convert. Game thread (BeginDeferred is GT-only); host-only.
void OnBeginDeferredSpawnObserve(void* /*context*/, void* srcObj, void* newActor) {
    auto* s = g_session.load(std::memory_order_acquire);
    // Phase 1 step 1A PROBE (read-only, CLIENT-side, before the host gate): record every keyless-family
    // load-spawn this thunk sees, so EmitVerdictAtQuiescence can test whether spawn-order catches every
    // surviving native (build plan 1A; docs/COOP_STABLE_ID_SIDECAR.md S3.2). Observe-only, mutates nothing.
    // Phase 1 step 2b BIND (CLIENT, before the host gate): same keyless-family classification feeds the 1A
    // probe (read-only) AND the eid-range bind (mutating) -- the bind binds the k-th keyless load-spawn to the
    // k-th received-map entry's host eid (save_identity_bind). Both are independently gated; one classification.
    if (newActor && s && s->connected() && s->role() == coop::net::Role::Client) {
        const bool probeOn = coop::dev::spawn_order_probe::IsEnabled();
        const bool bindOn = coop::save_identity_bind::IsEnabled();
        if (probeOn || bindOn) {
            // [18:45 wrong-type root fix 2026-07-04] A SAVE-LOAD spawn is a spawn from the GAMEMODE's
            // own frame: loadObjects/loadPrimitives run in mainGamemode's BP functions, so the thunk's
            // srcObj (FFrame::Object) is the gamemode instance (276 BeginDeferred call sites in the
            // gamemode BP). WITHOUT this gate, EVERY client chipPile spawn while connected consumed the
            // keyless ordinal cursor -- including our own wire-driven convert-LAND nativizations and
            // mirror spawns -- shifting every subsequent bind by one (client pile #k bound to host
            // eid #k+n): the mass identity misalignment behind the 18:45 wrong-type morphs. The k-th
            // GAMEMODE-sourced keyless spawn == primitivesData[k] by construction; nothing else is.
            // Class resolve is latched; a MISS is retried at most every 2s (perf-audit W-d: FindClass
            // is a full GUObjectArray walk -- an unthrottled null-retry would run it PER SPAWN while
            // mainGamemode_C is not yet loaded). Until it resolves, spawns are treated as non-load
            // (no cursor consumption) -- correct: a save-load spawn cannot precede its own gamemode.
            static void* sGmCls = nullptr;
            if (!sGmCls) {
                static std::chrono::steady_clock::time_point sNextGmClsTry{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= sNextGmClsTry) {
                    sNextGmClsTry = now + std::chrono::seconds(2);
                    sGmCls = R::FindClass(L"mainGamemode_C");
                }
            }
            void* srcCls = srcObj ? R::ClassOf(srcObj) : nullptr;
            const bool fromGamemode =
                srcCls && sGmCls && R::IsDescendantOfAny(srcCls, &sGmCls, 1);
            int fam = -1;  // 0 = chipPile, 1 = kerfurOff, -1 = not a keyless family / not a load spawn
            if (fromGamemode) {
                if (ue_wrap::prop::IsChipPile(newActor)) fam = 0;
                else if (void* c = R::ClassOf(newActor); c && coop::kerfur_entity::IsKerfurPropClass(c)) fam = 1;
            }
            if (fam >= 0) {
                if (probeOn)
                    coop::dev::spawn_order_probe::NoteKeylessSpawn(
                        newActor, fam == 0 ? coop::dev::spawn_order_probe::Family::ChipPile
                                           : coop::dev::spawn_order_probe::Family::KerfurOff);
                if (bindOn)
                    coop::save_identity_bind::OnSaveLoadSpawn(
                        newActor, fam == 0 ? coop::save_identity_map::Family::ChipPile
                                           : coop::save_identity_map::Family::KerfurOff);
            }
        }
    }
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;  // host authors converts
    if (!srcObj || !newActor) return;
    // GRAB direction (v106, 2026-07-07): EVERY clump is born from a chipPile's own
    // BeginDeferred (bytecode: actorChipPile toClump @141 + ubergraph @3493; srcObj =
    // the source pile, ALIVE at this POST -- it self-destructs after). Record the
    // birth certificate here: this is the DETERMINISTIC grab seam that fires for
    // E-press AND use-HOLD grabs (canBeUsedHold repeats with no new InpActEvt
    // dispatch -- the 10:19:03 mis-bind root: the InpActEvt PRE seam missed the
    // grab and the held-edge heuristic bound the clump to the WRONG open carry).
    // The held-edge (local_streams) consumes the certificate; the InpActEvt PRE
    // seam keeps only diagnostics + the client-side routing.
    if (ue_wrap::prop::IsChipPile(srcObj) && ue_wrap::prop::IsGarbageClump(newActor)) {
        coop::element::ElementId E = PT::GetPropElementIdForActor(srcObj);
        if (E == coop::element::kInvalidId)
            E = coop::remote_prop::ResolveMirrorEidByActor(srcObj);
        if (E == coop::element::kInvalidId) {
            // docs/piles/09 SELF-SEED (moved here from the E-press PRE, ONE owner):
            // an UNTRACKED pile grabbed in the post-blob purge gap mints its eid at
            // the very seam its clump is born (register-only, no broadcast).
            PT::MarkPropElement(srcObj, L"", R::ClassNameOf(srcObj));
            E = PT::GetPropElementIdForActor(srcObj);
            if (E != coop::element::kInvalidId)
                UE_LOGI("[PILE-09] HOST self-seeded UNTRACKED grabbed pile %p -> eid=%u "
                        "(thunk seam; eid-0-at-grab gap closed)",
                        srcObj, static_cast<unsigned>(E));
        }
        if (E != coop::element::kInvalidId) {
            // docs/piles/09: freeze the pile's PRE-GRAB position as the save-time key
            // (the pile has not moved yet -- it dies in place after this spawn).
            coop::save_transfer::RecordGrabTimePileXform(
                E, ue_wrap::engine::GetActorLocation(srcObj));
            coop::trash_channel::NoteClumpBorn(newActor, E, ue_wrap::prop::GetChipType(srcObj));
        }
        return;  // grab direction fully handled (the held-edge adopts)
    }
    if (!ue_wrap::prop::IsGarbageClump(srcObj)) return;   // re-pile source must be a clump
    if (!ue_wrap::prop::IsChipPile(newActor)) return;     // ... spawning a chipPile
    const coop::element::ElementId E = PT::GetPropElementIdForActor(srcObj);
    if (E == coop::element::kInvalidId) return;           // an UNTRACKED clump (grab-adopt miss; the eid=0 gap) -> skip
    // The thunk fires at the BeginDeferred POST, which is PRE-FinishSpawningActor -> newActor (the pile) is NOT
    // positioned yet: GetActorLocation(newActor) here is (0,0,0) and GetActorRotation is identity. take-31
    // proved this (host log: every "BROADCAST ToPile ... at (0.0,0.0,0.0)") -- reading newActor's transform
    // snapped every derived pile to the world origin = "disappeared". (take-30's rotation "worked" only because
    // an identity rotation LOOKS correct for a flat pile, masking the unset transform; loc came from the clump
    // then so it stayed visible.) So pass the CLUMP's transform (srcObj IS a fully-spawned, positioned actor)
    // as the FALLBACK, and re-read the PILE's REAL transform at the land-settle COMMIT (TickCarry, post-
    // FinishSpawning) in trash_channel -- that is where loc/rot/scale become valid. The clump's loc is
    // ~clump-radius high + its rotation is the tumble, so srcObj is ONLY the fallback (a committing settle
    // always has a live settled pile to re-read). [the original "not positioned yet" comment was RIGHT.]
    const ue_wrap::FVector  loc      = ue_wrap::engine::GetActorLocation(srcObj);
    const ue_wrap::FRotator rot      = ue_wrap::engine::GetActorRotation(srcObj);
    const uint8_t           chipType = ue_wrap::prop::GetChipType(srcObj);
    UE_LOGI("[PILE] HOST RE-PILE(thunk) eid=%u clump=%p -> chipPile=%p convert IN PLACE (deterministic, same "
            "tick as the spawn -- no death-watch, no proximity)", static_cast<unsigned>(E), srcObj, newActor);
    coop::trash_channel::OnHostConvert(*s, E, coop::net::propconvert_kind::kToPile, newActor, loc, rot, chipType);
}

// DIAGNOSTIC read-only thunk on AmainPlayer_C::dropGrabObject -- the PHC-GRAB RELEASE verb (RE-confirmed
// 2026-06-22, mainPlayer.json: it writes `grabbing_actor = NoObject` @167013 + `grabHandle->ReleaseComponent()`
// @167059). This is the seam for releasing a PHYSICS-HANDLE-grabbed object: the chipPile clump rides
// grabbing_actor, NOT holding_actor (confirmed -- neither chipPile nor clump references holding_actor). It
// catches BOTH a plain drop AND a throw: a throw runs throwShit (SetPhysicsLinearVelocity) + thrown() on the
// SAME tick immediately BEFORE dropGrabObject, so by this POST the clump already carries its throw velocity (no
// need to classify drop-vs-throw here -- the release edge reads whatever velocity the clump has). The earlier
// simulateDrop thunk was the EQUIPMENT drop (throwHoldingProp -> simulateDrop) and NEVER fired for the clump:
// SIM-DROP=0 across 6 grabs + drops + throws, the live disproof. RULE 2: simulateDrop retired, this is correct.
// PostNativeCallback fires AFTER dropGrabObject runs, so grabbing_actor is already null -> the ROBUST signal is
// AnyCarryingEid() (our latch, untouched by dropGrabObject): carrying == the carry-eid in [HELD-STATE] == the
// release fired for the carried clump. The FLIP will close the latch HERE (read-only logs only now).
//
// CONFIRM before the flip: (a) [DROP-GRAB] fires on the real drop AND throw with carrying=<the carry-eid>;
// (b) it does NOT fire DURING the carry (at a churn re-grab -- the held clump ptr changes ~every 3 s). If it
// fires on churn, the flip would close the latch mid-carry (regress the smooth carry) -> the close needs a gate.
void OnDropGrabObserve(void* /*context*/, void* player, void* /*result*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const coop::element::ElementId carryEid = coop::trash_channel::AnyCarryingEid();
    void* held = nullptr;
    coop::element::ElementId heldEid = coop::element::kInvalidId;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (player && ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) {
        held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;  // POST -> grabbing_actor already nulled by dropGrabObject
        if (held && R::IsLive(held)) heldEid = PT::GetPropElementIdForActor(held);
    }
    UE_LOGI("[DROP-GRAB] dropGrabObject fired player=%p grabbed=%p(POST) heldEid=%u carrying=%u -- READ-ONLY "
            "(cross-check carrying vs the carry-eid in [HELD-STATE]; MUST fire on a REAL drop/throw, NOT during "
            "the carry/churn; the FLIP closes the latch here -> the release edge then ships the throw velocity)",
            player, held,
            static_cast<unsigned>((heldEid  == coop::element::kInvalidId) ? 0u : heldEid),
            static_cast<unsigned>((carryEid == coop::element::kInvalidId) ? 0u : carryEid));
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
    // docs/piles/08 class-gate: a garbageClump is a host-authoritative trash entity -- its identity is the
    // source pile's eid E, bound by trash_channel::OnHostConvert at the convert-spawn POST; the held-pose
    // stream then carries E (local_streams). The clump must NEVER be expressed here as a FRESH-eid keyless
    // PropSpawn (that second cross-peer entity = the dupe). A clump that reaches here (a stray pickup, or a
    // pile that had no eid to convert) is simply not authored -- carry-only, no dupe.
    if (ue_wrap::prop::IsGarbageClump(heldActor)) return false;
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
    if (coop::join_membership_sweep::IsPendingSweepCandidate(heldActor)) {
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
    // CLIENT-ONLY (v105 root-fix companion, 2026-07-06): the divergence sweep is a CLIENT join
    // mechanism -- on the HOST g_sweepFired never flips, so this gate held FOREVER and silenced
    // every untracked keyed prop the host ever held (the never-mirrored-hotbar-hand log,
    // 12:40:05 'grabbed PRE-QUIESCENCE' on a 40-minute-old host session). The host's world IS
    // the reconciled authority by definition -- the un-adjudicated-local concept only exists on
    // a joining client. Hand items themselves no longer reach here (coop/player/hand_item), but
    // the express-if-unknown self-heal below must work for a host-grabbed world straggler.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced() &&
        coop::join_membership_sweep::IsInDivergenceUniverseUnclaimed(heldActor)) {
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
    coop::join_membership_sweep::RecordClaimIfTracking(heldActor);
    // (v81 MORPH V2: the former clump WatchClump enroll is GONE -- clumps return early above; the
    // bind-model pile_morph owns the clump's identity + its land conversion.)
    return true;
}

// ---- The client-grab INTERCEPTOR on InpActEvt_use (docs/piles/08) ------------------------------------
//
// DECISIVE RE (votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md): a chipPile grab is the E-press
// input action AmainPlayer_C::InpActEvt_use_..._41 -> icast(lookAtActor)->playerGrabbed (spawn clump +
// pickupObjectDirect(clump) + K2_DestroyActor(self)), all dispatched BP-internally (EX_LocalVirtualFunction
// -> ProcessInternal) and INVISIBLE to our ProcessEvent detour. InpActEvt_use itself IS ProcessEvent-visible
// (the native input->UFunction dispatch), and it is a thin stub: `CALL ExecuteUbergraph_mainPlayer(112867)`
// -- the whole use flow (the grab AND `useAction`, which plays `use_deny` on its fail branches) is that one
// event's ubergraph entry.
//
// This is a PRE-INTERCEPTOR (returns true -> the native InpActEvt_use dispatch is CANCELLED entirely, never
// reaching the ubergraph). On the CLIENT, whenever we route the press to the host (a pile GRAB or a carry
// THROW), we return true: the native grab AND its `use_deny` "EHHH" both die at the source (RE 2026-07-01 --
// nulling lookAtActor was a HALF-suppression: it killed the grab but `useAction` re-traces on its own and
// still hit a deny branch, playing use_deny in parallel with our synthesized pickup cue -- the suppression
// axis, not delivery). For any non-pile E-press (and for the HOST, which grabs natively) we return false ->
// the native use runs unchanged (devices, legit denies, the host's own grab). Puppets are unpossessed ->
// never process input -> this only ever fires for the local player.
// The "use" action has THREE delegate bindings (mainPlayer.json Export 483): _41 (IE_Pressed, the grab press we
// fully intercept below), _38 (a SECOND IE_Pressed), and _42 (IE_Released). All three reach useAction's use_deny
// "EHHH". Hooking only _41 left _38 firing on every E-press -> the native deny played in PARALLEL with our
// cancelled grab (the 19:06 regression -- RE 2026-07-01: the deny is on a SEPARATE seam than the one _41
// cancels). This SIDE-EFFECT-FREE suppressor covers the SECOND PRESS seam (_38): on a CLIENT pile interaction
// it only CANCELS the dispatch (kills the parallel deny) -- it does NOT send a grab/throw intent or play a cue
// (_41 is the sole author of those). Same recognition as OnPileUseIntercept's client branch: carrying, or aimed
// at ANY native pile (bound or unbound -- the 2026-07-03 twin-grab gate) / proxy pile. On cancel it ARMS the
// paired-release latch (a cancelled press's _42 must die
// with it -- see g_cancelPairedUseRelease; the RELEASE seam is pairing-only, NOT condition-derived).
static bool OnPileUseDenySuppress(void* self, void* /*params*/) {
    if (!self) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return false;  // client-only; host runs native
    bool cancel = false;
    if (coop::trash_channel::ClientCarryEid() != coop::element::kInvalidId) {
        cancel = true;  // carrying a clump -> this press is the throw toggle -> the native press would deny
    } else {
        void* aimedNative = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
        if (aimedNative && ue_wrap::prop::IsChipPile(aimedNative)) {
            // ANY native pile aim -- bound OR unbound -- mirrors _41's shape (audit
            // 45bdb7ac W-2): _41 cancels the unbound press too now (the twin-grab
            // gate), so _38 must die with it or the native deny "EHHH" plays alone.
            cancel = true;
        } else {
            const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
            const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
            const float d2r = 3.14159265f / 180.f;
            const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
            const float cp = std::cos(pitch);
            const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
            cancel = coop::trash_proxy::EidForAimedPileProxy(camLoc, camFwd, /*maxRangeCm=*/400.f,
                                                             /*minDot=*/0.94f) != coop::element::kInvalidId;
        }
    }
    if (cancel) g_cancelPairedUseRelease = true;
    return cancel;  // false: not a pile interaction -> native use runs (devices, other interactions, SP deny)
}

// The IE_Released seam (_42) is PAIRING-ONLY: cancel iff this release's press was cancelled (by _41's
// interceptor or _38's suppressor -- either arms the latch). NO condition re-derivation here: the press
// conditions are stale by release time (the throw press already cleared the carry; the post-drop aim
// wanders), and matching conditions against a NATIVE press's release would eat a legit drop (e.g.
// releasing a natively-held prop while aiming at a pile). One latch consume per gesture; user-rate.
static bool OnPileUseReleaseSuppress(void* /*self*/, void* /*params*/) {
    if (!g_cancelPairedUseRelease) return false;  // paired with a NATIVE press -> the native release must run
    g_cancelPairedUseRelease = false;
    UE_LOGI("[USE-RELEASE] paired E-release CANCELLED (its press was intercepted -- no use_deny on release)");
    return true;
}

static bool OnPileUseIntercept(void* self, void* /*params*/) {
    if (!self) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;  // no coop session -> native use runs normally (SP / disconnected)

    // CLIENT (Increment 2, the client-grab direction): the client has NO real actorChipPile_C -- every pile it
    // sees is an AStaticMeshActor PROXY (trash_proxy). Recognition is a CAMERA-RAY CONE (EidForAimedPileProxy):
    // the most-centered pile proxy within range of the aim ray. (The earlier "give the proxy collision so the
    // game's TraceTypeQuery1 trace hits it + read lookatActorCurrent" approach was DISPROVEN by the harness --
    // the worn pile mesh has no simple collision body, so the trace never hit; RULE 2, retired 2026-06-23. A
    // future garbageCollider-analog SHAPE component is the faithful fix for both occlusion-correct aim AND the
    // walk-through-pile movement-block.) NO suppress-native needed: the proxy fails every native int_player_C
    // cast on E-press, so the local grab no-ops on its own. The host re-validates + enforces one-hold-per-peer.
    if (s->role() != coop::net::Role::Host) {
        // TOGGLE: carrying a clump already? an E-press is a THROW (release it, regardless of aim). Else, if
        // aimed at a mirrored pile, an E-press is a GRAB request. (A player holds at most one trash clump.)
        const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
        if (carry != coop::element::kInvalidId) {
            // Self-heal (belt-and-suspenders for the audit HIGH): if we think we're carrying but the proxy is
            // gone (a missed/dropped host abort edge), clear the stale toggle + fall through to a fresh grab
            // rather than throwing a dead eid forever.
            if (!coop::trash_proxy::ProxyActorForEid(carry)) {
                UE_LOGW("[THROW-INTENT] CLIENT carry eid=%u has NO live proxy -- stale toggle, clearing + falling "
                        "through to grab", static_cast<unsigned>(carry));
                coop::trash_channel::ClearClientCarry(static_cast<uint32_t>(carry));
            } else {
                UE_LOGI("[THROW-INTENT] CLIENT E-PRESS while carrying eid=%u -> requesting release(E-drop) from host "
                        "(native use CANCELLED -- no use_deny)", static_cast<unsigned>(carry));
                coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry),
                                                     coop::net::throw_mode::kRelease, ue_wrap::FVector{});
                g_cancelPairedUseRelease = true;  // this press's _42 release must die with it (deny lives there too)
                return true;  // handled: cancel the native InpActEvt_use (the client holds no native clump -> would deny)
            }
        }
        // (X) native-authoritative GRAB recognition (items 5+6). A save-loaded pile the client BOUND as the
        // host-range mirror (save_identity_bind) is a REAL actorChipPile_C native -> it IS the game's own
        // lookAtActor (the int_player_C interaction trace), occlusion-correct + collision-blocked, unlike a
        // bare proxy. PREFER it: read lookAtActor; if it's a bound native pile, CANCEL the whole native
        // InpActEvt_use dispatch (return true below) -- the ubergraph's grab (icast(lookAtActor)->playerGrabbed)
        // AND `useAction`'s `use_deny` both die at the source. (RULE 2, 2026-07-01: the old NULL-lookAtActor
        // suppression is RETIRED -- it was a HALF-suppression that killed the grab but let useAction re-trace
        // and play use_deny "EHHH" in parallel; cancelling the dispatch subsumes it, no field mutation.) Route
        // the grab via GrabIntent (host-auth, the sole author of the shared mutation; the host runs
        // playerGrabbed on the requester's puppet). The camera-cone below stays only for UNBOUND proxy piles.
        {
            void* aimedNative = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
            if (aimedNative && ue_wrap::prop::IsChipPile(aimedNative)) {
                if (PT::IsBoundMirrorNative(aimedNative)) {
                    const coop::element::ElementId beid = coop::remote_prop::ResolveMirrorEidByActor(aimedNative);
                    if (beid != coop::element::kInvalidId) {
                        // Pickup cue: the native grab (which plays the `use` click + material soft cue in its
                        // playerGrabbed chain) is CANCELLED for this press, so synthesize the same feedback locally
                        // at the pile -- the grabber hears its own grab (the host hears it natively via playerGrabbed
                        // on the puppet; observers via ResolveAndStartDrive).
                        coop::prop_sound::PlayUseClick(aimedNative);
                        coop::prop_sound::PlayGrabSound(aimedNative);
                        // [PILE-TYPE probe 2026-07-04] pos+chipType of the pile the CLIENT is aiming at --
                        // the host's "[GRAB-INTENT] EXEC ... at(...) chipType=" for the same eid must match;
                        // a mismatch names the ordinal identity misalignment (the 18:45 wrong-type morph).
                        const ue_wrap::FVector cloc = ue_wrap::engine::GetActorLocation(aimedNative);
                        UE_LOGI("[GRAB-INTENT] CLIENT E-PRESS on BOUND native pile eid=%u at(%.1f,%.1f,%.1f) "
                                "chipType=%u (lookAtActor, occlusion-correct) -> native use CANCELLED "
                                "(no grab, no use_deny) + requesting grab from host",
                                static_cast<unsigned>(beid), cloc.X, cloc.Y, cloc.Z,
                                static_cast<unsigned>(ue_wrap::prop::GetChipType(aimedNative)));
                        coop::trash_channel::SendGrabIntent(*s, static_cast<uint32_t>(beid));
                        g_cancelPairedUseRelease = true;  // pair: the _42 release of this cancelled press dies too
                        return true;  // handled: cancel the native InpActEvt_use dispatch entirely
                    }
                }
                // UNBOUND native pile (2026-07-03, the 11:49 client-local clump chain): a REAL
                // actorChipPile_C in the client's aim that NO eid owns -- a HOST-VACATE twin the
                // sweep has not retired yet (the 11:48:51 seed: twin@old grabbed 7 s before the
                // sweep pass), a mid-bind-window native, or a descendant of an earlier native
                // grab. Letting the native use run here is how the self-perpetuating client-only
                // chain seeds: native grab -> local prop_garbageClump_C -> native land -> another
                // unbound pile, all invisible to the host (and a grabbed twin becomes a CLUMP the
                // pile sweep can no longer retire). On a connected client EVERY pile interaction
                // is host-authoritative -- a pile no eid owns must not be interactable at all.
                if (coop::join_membership_sweep::HasLoadTailQuiesced()) {
                    // v106 (2026-07-07 ghost-pile fix): POST-quiescence, an unbound pile is
                    // PROVABLY identity-less -- every host identity was bound/expressed by the
                    // sweep, and nothing on the wire can ever address a native with no eid.
                    // "It binds or retires shortly" never happens here (the 10:19:27 wedge:
                    // the same pile pressed twice, 8s+ after the sweep, forever inert) --
                    // a bind-displaced leftover. Retire it: echo-suppressed deferred destroy.
                    UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p POST-quiescence "
                            "-- identity-less ghost (no eid can ever address it) -> retiring it "
                            "locally (echo-suppressed destroy)", aimedNative);
                    coop::prop_lifecycle::DestroyLocalProp(aimedNative, /*deferred=*/true);
                    g_cancelPairedUseRelease = true;
                    return true;
                }
                // Pre-quiescence: the bind window is still open -- CANCEL the press (the
                // pile binds or retires within seconds; a re-press then routes normally).
                UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p -- native use CANCELLED "
                        "(no eid owns it: bind-window; it binds or retires shortly, re-press then)",
                        aimedNative);
                g_cancelPairedUseRelease = true;
                return true;
            }
        }
        // Aim ray from the live view camera (FALLBACK -- unbound proxy piles only). Forward from the camera
        // rotation (deg->rad): the unit vector the cone tests each pile proxy against. maxRange 400 cm (a
        // generous reach), minDot 0.94 (~20 deg cone -- forgiving so a slightly-off aim still grabs).
        const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
        const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
        const float d2r = 3.14159265f / 180.f;
        const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
        const float cp = std::cos(pitch);
        const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
        const coop::element::ElementId eid =
            coop::trash_proxy::EidForAimedPileProxy(camLoc, camFwd, /*maxRangeCm=*/400.f, /*minDot=*/0.94f);
        if (eid == coop::element::kInvalidId)
            return false;  // not aiming at a mirrored pile -> let the native use run (devices, other interactions)
        // Pickup cue at the aimed proxy pile (same as the bound-native branch -- the native use is CANCELLED
        // below, so the grabber hears nothing without this). ProxyActorForEid gives the proxy actor.
        if (void* proxyActor = coop::trash_proxy::ProxyActorForEid(eid)) {
            coop::prop_sound::PlayUseClick(proxyActor);
            coop::prop_sound::PlayGrabSound(proxyActor);
        }
        UE_LOGI("[GRAB-INTENT] CLIENT E-PRESS aimed at pile proxy eid=%u (camera-ray cone) -> native use CANCELLED "
                "(no use_deny) + requesting grab from host", static_cast<unsigned>(eid));
        coop::trash_channel::SendGrabIntent(*s, static_cast<uint32_t>(eid));
        g_cancelPairedUseRelease = true;  // pair: the _42 release of this cancelled press dies too
        return true;  // handled: cancel the native InpActEvt_use dispatch (a bare proxy would deny)
    }

    // HOST: aim at a REAL chipPile (it implements int_player_C -> it IS lookAtActor). The host ALWAYS runs the
    // native use (it grabs natively) -- this branch only records the pending grab; every host path returns false.
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the pile (PRE-conversion, alive)
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return false;   // E-press not aimed at a pile -> native use runs
    // The HOST grab syncs WITHOUT arming anything here -- the BP's own BeginDeferred(clump) is caught at
    // the thunk's GRAB direction (the v106 birth certificate); the held-edge adopts the spawned clump onto
    // the pile's eid + broadcasts PropConvert{ToClump}. This branch logs diagnostics only.
    coop::element::ElementId fwdEid = PT::GetPropElementIdForActor(aimed);
    coop::element::ElementId mirEid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    ue_wrap::engine::MainPlayerGrabState gs{};
    ue_wrap::engine::ReadMainPlayerGrabState(self, gs);
    const unsigned fwd = (fwdEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(fwdEid);
    const unsigned mir = (mirEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(mirEid);
    UE_LOGI("[PILE] HOST E-PRESS on pile %p -- localEid=%u mirrorEid=%u %s (carry slots: grabbingActor=%p "
            "holdingActor=%p)",
            aimed, fwd, mir,
            (fwd != 0 || mir != 0) ? "[TRACKED -> grab will sync]"
                                   : "[UNTRACKED -> grab will NOT sync; tracking gap]",
            gs.grabbingActor, gs.holdingActor);
    // (v106: the self-seed + pre-grab-xform record + pending-grab arm MOVED to the
    // BeginDeferred thunk's GRAB direction -- ONE owner at the seam the clump is
    // actually born, covering use-HOLD grabs this press seam never sees. This host
    // branch is diagnostics-only now.)
    return false;  // HOST: never cancel -- the native grab must run
}

// (X) LMB hard-throw bridge (2026-06-26). The NATIVE throw input is InpActEvt_fire (LMB) -> throwHoldingProp
// -> throwShit(velocity_fromCamera) -- a SEPARATE input from use/E (which is grab + the E-drop release). The
// client holds NO native clump (host-authoritative; it renders a proxy), so the native fire's throwHoldingProp
// finds no grabbing_component and no-ops -- and we never routed it, hence LMB did nothing (the 12:35 symptom).
// Here: while carrying a clump proxy, an LMB press requests a hardThrow with the client's INSTANTANEOUS camera-
// forward (Variant B -- the throw flies exactly where the client looks at the press, native-faithful); the host
// applies the native velocity formula (cameraFwd * 15000/max(mass,10) + puppetVel, NO cap) with the real clump
// mass. The native fire is left to run (it no-ops with no held native prop; nothing to suppress -- there is no
// single lookAtActor-style field, and the client holds no equipment while carrying a clump). Client-only: the
// host throws natively via its own InpActEvt_fire. E-drop (the #3 path) is untouched.
static void OnFirePre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) return;          // the host throws natively; this is the client bridge
    const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
    if (carry == coop::element::kInvalidId) return;          // not carrying a clump -> let native fire do its thing
    if (!coop::trash_proxy::ProxyActorForEid(carry)) {       // stale toggle (proxy gone) -> clear + let native run
        coop::trash_channel::ClearClientCarry(static_cast<uint32_t>(carry));
        return;
    }
    // The client's instantaneous camera-forward unit vector (same derivation as the grab cone). The native
    // throw is camera-DIRECTED, so this is the authoritative aim; the host applies the mass-scaled speed.
    const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
    const float d2r = 3.14159265f / 180.f;
    const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
    const float cp = std::cos(pitch);
    const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
    UE_LOGI("[THROW-INTENT] CLIENT LMB(fire) while carrying eid=%u -> requesting NATIVE hard-throw from host "
            "(camFwd=%.2f,%.2f,%.2f)", static_cast<unsigned>(carry), camFwd.X, camFwd.Y, camFwd.Z);
    coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry),
                                         coop::net::throw_mode::kHardThrow, camFwd);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)

    // The deterministic re-pile thunk (READ-ONLY observe pass this cut): patch
    // BeginDeferredActorSpawnFromClass's UFunction::Func so we catch the clump's chipPile spawn
    // (EX_CallMath -> invisible to ProcessEvent). Independent latch -> retries until GameplayStatics
    // resolves, regardless of the grab-observer state. host_spawn_watcher's POST observer on the SAME
    // UFunction is unaffected (separate mechanism; our forwarder is transparent + the pinecone path's
    // result is not a chipPile/clump, so our cb ignores it).
    if (!g_repileThunkInstalled) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        void* bdFn  = gsCls ? R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn) : nullptr;
        if (bdFn) {
            ue_wrap::ufunction_hook::InstallPostHook(bdFn, &OnBeginDeferredSpawnObserve);
            g_repileThunkInstalled = true;  // latch on attempt (InstallPostHook logs success/refusal)
            UE_LOGI("trash_collect: re-pile Func-patch armed on BeginDeferred -- the DETERMINISTIC converter "
                    "(clump re-pile -> OnHostConvert ToPile the same tick as the spawn; death-watch retired)");
        }
        // bdFn null -> GameplayStatics not loaded yet; retry next world-gated Install call.
    }

    // DIAGNOSTIC read-only: Func-thunk on AmainPlayer_C::dropGrabObject -- the PHC-GRAB RELEASE verb (the
    // correct seam; the prior simulateDrop thunk was the EQUIPMENT drop and NEVER fired for the PHC-grabbed
    // clump -- SIM-DROP=0 live, RULE 2 retired). dropGrabObject writes grabbing_actor = NoObject +
    // grabHandle->ReleaseComponent(); it is FUNC_BlueprintEvent dispatched EX_LocalVirtualFunction -> below
    // ProcessEvent, so a PRE observer can't see it -> the Func-thunk catches it. Logs carrying eid; the FLIP
    // will close the carry latch here so a real drop/throw lets the release edge ship the throw velocity.
    if (!g_dropGrabThunkInstalled) {
        void* pcls = R::FindClass(P::name::MainPlayerClass);
        void* dgFn = pcls ? R::FindFunction(pcls, L"dropGrabObject") : nullptr;
        if (dgFn) {
            ue_wrap::ufunction_hook::InstallPostHook(dgFn, &OnDropGrabObserve);
            g_dropGrabThunkInstalled = true;
            UE_LOGI("trash_collect: read-only dropGrabObject thunk armed (the PHC-grab release seam -- logs "
                    "carrying eid; drop AND throw both route through dropGrabObject; confirm it fires on a REAL "
                    "release NOT on a churn re-grab before the flip)");
        }
        // dgFn null -> mainPlayer_C not loaded yet; retry next world-gated Install call.
    }

    if (g_grabObserverInstalled) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;  // mainPlayer_C not loaded yet -> retry on the next world-gated Install
    void* fn = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("trash_collect: InpActEvt_use UFunction not found -- pile grabs cannot drop peer mirrors");
        g_grabObserverInstalled = true;  // permanent give-up (don't re-walk the class forever)
        return;
    }
    if (!GT::RegisterInterceptor(fn, &OnPileUseIntercept)) {
        UE_LOGW("trash_collect: InpActEvt_use PRE interceptor register failed (table full?)");
        return;  // not latched -> retry next Install
    }
    g_grabObserverInstalled = true;
    UE_LOGI("trash_collect: pile use INTERCEPTOR installed on InpActEvt_use (host: records the pending grab, "
            "always runs native; client: cancels the native use for a pile GRAB/THROW -> no native grab, no "
            "use_deny 'EHHH'; routes GrabIntent/ThrowIntent to the host; docs/piles/08)");

    // The "use" action's OTHER two bindings (_38 = 2nd IE_Pressed, _42 = IE_Released) also reach useAction's
    // use_deny -- hooking only _41 left the client hearing "EHHH" on every E-press (parallel deny). _38 gets the
    // press-condition suppressor; _42 gets the PAIRING-ONLY release suppressor (2026-07-02: deriving the press
    // conditions at release time both leaked the deny on E-drop -- the throw press had already cleared the carry
    // -- and could eat a legit native release). Best-effort (a missing ordinal after a BP recook just means the
    // deny returns on that seam -- sdk_check flags it).
    int denySeams = 0;
    if (void* uf38 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn38))
        if (GT::RegisterInterceptor(uf38, &OnPileUseDenySuppress)) ++denySeams;
    if (void* uf42 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn42R))
        if (GT::RegisterInterceptor(uf42, &OnPileUseReleaseSuppress)) ++denySeams;
    UE_LOGI("trash_collect: use_deny suppressors installed on %d/2 extra 'use' seam(s) (_38 2nd-Pressed = press "
            "conditions; _42 Released = paired-with-cancelled-press only) -- a cancelled client E-press now dies "
            "on ALL its seams incl. its OWN release -> no 'EHHH' on grab OR drop",
            denySeams);

    // LMB hard-throw bridge: PRE-observe BOTH fire handlers (_58/_59 = press/release). The OnFirePre gate on
    // ClientCarryEid + SendThrowIntent's optimistic carry-clear mean only the press edge that finds a live
    // carry acts; the other no-ops. Best-effort (a missing handler just means no LMB-throw on that edge).
    int fireObs = 0;
    for (const wchar_t* fireFn : { P::name::MainPlayerFireInputEventFn58, P::name::MainPlayerFireInputEventFn59 }) {
        void* ff = R::FindFunction(cls, fireFn);
        if (ff && GT::RegisterPreObserver(ff, &OnFirePre)) ++fireObs;
    }
    UE_LOGI("trash_collect: LMB hard-throw observer installed on %d/2 InpActEvt_fire handler(s) "
            "(client routes LMB-while-carrying -> native camera-driven ThrowIntent)", fireObs);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // Session teardown resets the gesture latch (symmetry with trash_channel's client
    // toggles): a press cancelled just before the disconnect must not eat a release
    // dispatched in the next session (audit hygiene 2026-07-02).
    g_cancelPairedUseRelease = false;
}

bool DebugSendGrabIntent(uint32_t eid) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) {
        UE_LOGW("trash_collect: DebugSendGrabIntent eid=%u -- no running client session", eid);
        return false;
    }
    coop::trash_channel::SendGrabIntent(*s, eid);
    return true;
}

bool DebugSendThrowIntent(uint32_t eid) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) {
        UE_LOGW("trash_collect: DebugSendThrowIntent eid=%u -- no running client session", eid);
        return false;
    }
    coop::trash_channel::SendThrowIntent(*s, eid, coop::net::throw_mode::kRelease, ue_wrap::FVector{});
    return true;
}

}  // namespace coop::trash_collect_sync
