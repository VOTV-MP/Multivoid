// coop/props/trash_collect_sync.cpp -- the held-item express and the pile seams.
// EnsureHeldItemBroadcast runs on the pump's new-held edge: a freshly spawned, auto-grabbed prop
// with no Key gets a stable Key and a PropSpawn, and the pose stream then mirrors it into the
// collector's hands. The BeginDeferred Func patch is the deterministic grab and re-pile seam for
// chip piles. See coop/props/trash_collect_sync.h.

#include "coop/props/trash_collect_sync.h"

#include "coop/dev/spawn_order_probe.h"  // the client load-spawn coverage probe
#include "coop/props/save_identity_bind.h"     // the client eid bind
#include "coop/props/save_identity_map.h"      // Family
#include "coop/creatures/kerfur_entity.h"  // IsKerfurActor, IsKerfurPropClass
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/quiescence_drain.h"   // ArmGhostSweep
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_sound.h"           // client-own-grab pickup cue (native grab suppressed -> synthesize locally)
#include "coop/props/prop_synth_key.h"
#include "coop/props/remote_prop.h"        // ResolveMirrorEidByActor (the pile-grab hook mirror eid resolve)
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // the sweep's candidate and claim queries
#include "coop/save/save_transfer.h"      // RecordGrabTimePileXform, the grab-edge save-time key
#include "coop/props/trash_channel.h"      // NoteClumpBorn, the clump's birth certificate
#include "coop/props/trash_proxy.h"        // EidForAimedPileProxy
#include "coop/props/trash_use_intercept.h"  // the InpActEvt_use client-grab interceptor
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"     // RegisterPreObserver (the InpActEvt_use pile-grab observer)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"     // MainPlayerClass + MainPlayerUseInputEventFn
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"  // the BeginDeferred Func patch, the deterministic re-pile

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

// The cached Session for the observers (a PRE callback takes no parameter); re-cached by every
// Install for a reconnect. Read on the game thread.
std::atomic<coop::net::Session*> g_session{nullptr};

// The host-grab identity travels by the clump's birth certificate (NoteClumpBorn at the
// BeginDeferred seam), and the held edge adopts the clump onto that eid; identity is the host
// eid end to end, never a position. The chipPile and clump spawns dispatch EX_CallMath,
// invisible to the ProcessEvent hook, which is why the seam is a Func patch.

// A consumed clump (no re-pile spawn) dies untracked, and the prop reaper broadcasts its
// destroy.

bool g_repileThunkInstalled = false;          // process-lifetime Func-patch latch

// The deterministic re-pile. A host re-pile is the clump's own BeginDeferredActorSpawnFromClass
// (self the clump, class the pile), which fires this Func patch with the re-piling clump as the
// source object and the new pile as the result. If the clump is a tracked trash entity, its eid
// is converted onto the new pile in place, the same tick the pile is constructed: no proximity,
// no death watch, no reaper race. The grab direction (source a pile, result a clump) is
// recorded here too and adopted at the held edge. Game thread; the convert is host-only.
void OnBeginDeferredSpawnObserve(void* /*context*/, void* srcObj, void* newActor) {
    auto* s = g_session.load(std::memory_order_acquire);
    // On a client, before the host gate: every keyless-family load spawn this seam sees feeds the
    // spawn-order coverage probe (read-only) and the eid bind (the k-th keyless load spawn binds to
    // the k-th received map entry). Both are independently gated; one classification.
    if (newActor && s && s->connected() && s->role() == coop::net::Role::Client) {
        const bool probeOn = coop::dev::spawn_order_probe::IsEnabled();
        const bool bindOn = coop::save_identity_bind::IsEnabled();
        if (probeOn || bindOn) {
            // A save-load spawn is a spawn from the gamemode's own frame: the load functions run in
            // mainGamemode's BP functions, so the source object is the gamemode. Without this gate
            // every client chipPile spawn while connected (our own wire-driven landings and mirror
            // spawns included) consumed the keyless cursor and shifted every later bind by one. The
            // class resolve is latched, and a miss is retried at most every 2 s (FindClass is a
            // full walk; an unthrottled retry would run it per spawn); until it resolves, spawns
            // count as non-load, which is correct, since a save-load spawn cannot precede its own
            // gamemode.
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
    // The grab direction: every clump is born from a chipPile's own BeginDeferred, with the source
    // pile alive at this point (it self-destructs after). The birth certificate is recorded here,
    // the seam that fires for an E-press and a use-hold grab alike (a hold repeats with no new
    // input dispatch, which the input seam misses); the held edge consumes the certificate, and the
    // input seam keeps only diagnostics and the client routing.
    if (ue_wrap::prop::IsChipPile(srcObj) && ue_wrap::prop::IsGarbageClump(newActor)) {
        coop::element::ElementId E = PT::GetPropElementIdForActor(srcObj);
        if (E == coop::element::kInvalidId)
            E = coop::remote_prop::ResolveMirrorEidByActor(srcObj);
        if (E == coop::element::kInvalidId) {
            // The self-seed, one owner: an untracked pile grabbed in the post-load purge gap mints
            // its eid at the seam its clump is born (register only, no broadcast).
            PT::MarkPropElement(srcObj, L"", R::ClassNameOf(srcObj), PT::EnrollSource::kExpressSeam);
            E = PT::GetPropElementIdForActor(srcObj);
            if (E != coop::element::kInvalidId)
                UE_LOGI("[PILE-09] HOST self-seeded UNTRACKED grabbed pile %p -> eid=%u "
                        "(thunk seam; eid-0-at-grab gap closed)",
                        srcObj, static_cast<unsigned>(E));
        }
        if (E != coop::element::kInvalidId) {
            // The pile's pre-grab position frozen as the save-time key (it has not moved; it dies
            // in place after this spawn).
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
    // This fires after BeginDeferred and before FinishSpawningActor, so the new pile is not
    // positioned yet (its location reads as the origin and its rotation as identity); the clump's
    // transform is passed as the fallback (a fully spawned actor), and the pile's real transform is
    // re-read at the land-settle commit in trash_channel, after FinishSpawning. The clump's
    // location is a radius high and its rotation the tumble, so it is only the fallback.
    const ue_wrap::FVector  loc      = ue_wrap::engine::GetActorLocation(srcObj);
    const ue_wrap::FRotator rot      = ue_wrap::engine::GetActorRotation(srcObj);
    const uint8_t           chipType = ue_wrap::prop::GetChipType(srcObj);
    UE_LOGI("[PILE] HOST RE-PILE(thunk) eid=%u clump=%p -> chipPile=%p convert IN PLACE (deterministic, same "
            "tick as the spawn -- no death-watch, no proximity)", static_cast<unsigned>(E), srcObj, newActor);
    coop::trash_channel::OnHostConvert(*s, E, coop::net::propconvert_kind::kToPile, newActor, loc, rot, chipType);
}

}  // namespace

bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* s) {
    if (!heldActor || !s || !s->connected()) return false;
    if (!R::IsLive(heldActor)) return false;
    // The kerfur gate: a kerfur prop is a host-owned entity, and a client must never mint a
    // peer-range eid for it (the host's conversion gate dropped such a mint and re-mirrored it as a
    // fresh entity: the dupe-and-drop loop). The held-pose stream carries the kerfur prop mirror by
    // its host-range eid instead, which the host resolves and drives kinematically. On the host
    // this is redundant (its kerfur prop is tracker-known), but the gate is role-agnostic.
    if (coop::kerfur_entity::IsKerfurActor(heldActor)) return false;
    // The clump gate: a garbageClump is a host-authoritative trash entity whose identity is the
    // source pile's eid, bound at the convert seam and carried by the held-pose stream. Expressed
    // here as a fresh-eid keyless spawn it would be a second cross-peer entity, so a stray clump is
    // carry-only.
    if (ue_wrap::prop::IsGarbageClump(heldActor)) return false;
    // Any keyed interactable, the Aprop_C items and the non-Aprop_C clump and pile alike. Crash
    // safety is on the receiver: GetStaticMesh is null for a non-Aprop_C, so the peer never runs
    // physics on a clump mirror, which is driven kinematically.
    if (!ue_wrap::prop::IsKeyedInteractable(heldActor)) return false;
    // The client host-authority gate for shared trash (the chipPile, the clump, the dispenser
    // pile): host-owned entities. A client grabbing a host pile morphs it locally to a clump on an
    // unhookable path, and expressing that clump would fresh-spawn a host duplicate, whose later
    // landing would spawn yet another pile. So a client never authors shared trash: its grab
    // already removed the host's original through the grab observer's destroy, and it holds the
    // clump locally; a client-thrown ground pile staying client-local is a benign divergence, not a
    // dupe. Aprop_C items are unaffected, and the host authors its own held trash below.
    if (s->role() == coop::net::Role::Client && !ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        UE_LOGI("trash_collect: CLIENT holds shared trash cls='%ls' -- NOT authoring it (host owns world "
                "piles/clumps; the grab's PropDestroy already removed the host original). No dupe.",
                R::ClassNameOf(heldActor).c_str());
        return false;
    }

    // A divergence sweep is pending and this held actor is one of its candidates (an unclaimed
    // save-loaded local the host did not express, a ghost awaiting adjudication): expressing it
    // would fresh-spawn a host duplicate and claim the ghost past the sweep. A client-originated
    // drop is claimed on its own path and is not a candidate.
    if (coop::join_membership_sweep::IsPendingSweepCandidate(heldActor)) {
        UE_LOGI("trash_collect: held item %p is a pending divergence-sweep candidate "
                "(unclaimed ghost) -- NOT expressing (the deferred sweep will destroy it)", heldActor);
        return false;
    }

    // The pre-quiescence join window, client only. The guard above fires only once the sweep is
    // armed, but a save-loaded in-universe prop grabbed before the snapshot bracket opens slips
    // past it, and expressing it spawned a permanent host duplicate. Until the load tail quiesces
    // the world is not reconciled, so an un-adjudicated local is not ours to announce: the host
    // expresses its own copy in the bracket (ours is then matched and claimed), or the sweep
    // destroys ours. The host's world is the reconciled authority, so the gate never holds there.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced() &&
        coop::join_membership_sweep::IsInDivergenceUniverseUnclaimed(heldActor)) {
        UE_LOGI("trash_collect: held item %p cls='%ls' grabbed PRE-QUIESCENCE (join window not yet "
                "reconciled) -- NOT expressing (host expresses its own / the sweep adjudicates ours; "
                "prevents the off+grab host dupe)", heldActor, R::ClassNameOf(heldActor).c_str());
        return false;
    }

    // Express if unknown. "Has a key" is not "the peer has it": a prop that materialised in the
    // load's late tail is keyed yet unknown to the wire (never expressed, never bound), and the
    // player carrying one was invisible to the host. The skip test is tracker-known: a prop with a
    // live Element is genuinely shared and the pose stream alone mirrors it; an untracked one is
    // expressed here, key and all, the moment a player first touches it.
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(heldActor);
    if (!keyStr.empty() && keyStr != L"None" &&
        PT::GetPropElementIdForActor(heldActor) != coop::element::kInvalidId) {
        // Logged with key and eid: "the pose stream suffices" holds only while the prop is actively
        // held and streamed, so a repro can tell a tracker-known decline from a never-received
        // spawn.
        UE_LOGI("[ROCK-DROP] EnsureHeldItemBroadcast DECLINE (tracker-known): cls='%ls' key='%ls' eid=%u "
                "-- 'pose stream suffices' assumes an active stream; false if the prop is no longer held",
                R::ClassNameOf(heldActor).c_str(), keyStr.c_str(),
                static_cast<unsigned>(PT::GetPropElementIdForActor(heldActor)));
        return false;  // keyed AND tracker-known: the peer has it; pose stream suffices
    }

    const std::wstring cls = R::ClassNameOf(heldActor);
    // Aprop_C items get a force-minted Key (their construction script holds it). The non-Aprop_C
    // clump is not keyable (setKey does not stick), so it rides the same pipeline identified by
    // eid: broadcast with no key, and the receiver resolves its mirror by eid. The clump renders on
    // its own, so no mesh transfer is needed.
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
    // The Prop Element shadow and the dedupe latch, so the item's eventual destroy unwinds through
    // the normal path.
    PT::MarkProcessedInit(heldActor);
    PT::MarkPropElement(heldActor, keyStr, cls, PT::EnrollSource::kExpressSeam);

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
    // The real scale, and the identity row below.
    const ue_wrap::FVector scl = ue_wrap::engine::GetActorScale3D(heldActor);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // The trash variant, so the mirror shows the same chip or clump type; 0 for a non-trash actor.
    p.chipType = ue_wrap::prop::GetChipType(heldActor);
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    // The physics flags read Aprop_C offsets, so only on an Aprop_C; the receiver ignores them for
    // a kinematic clump mirror.
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        if (ue_wrap::prop::IsHeavy(heldActor))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(heldActor)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(heldActor)) p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(heldActor)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(heldActor)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        // The identity row: a held generic prop mirrors as itself, not as the class default.
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
    // The self-claim: this peer just expressed the held item, and an open bracket's sweep must not
    // destroy it as unclaimed.
    coop::join_membership_sweep::RecordClaimIfTracking(heldActor);
    return true;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)

    // The Func patch on BeginDeferredActorSpawnFromClass, so the clump's chipPile spawn (an
    // EX_CallMath, invisible to ProcessEvent) is caught. Its own latch, retried until
    // GameplayStatics resolves; host_spawn_watcher's observer on the same UFunction is a separate
    // mechanism, and the forwarder is transparent.
    if (!g_repileThunkInstalled) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        void* bdFn  = gsCls ? R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn) : nullptr;
        if (bdFn) {
            ue_wrap::ufunction_hook::InstallPostHook(bdFn, &OnBeginDeferredSpawnObserve);
            g_repileThunkInstalled = true;  // latched on the attempt; InstallPostHook logs success or refusal
            UE_LOGI("trash_collect: re-pile Func-patch armed on BeginDeferred -- the DETERMINISTIC converter "
                    "(clump re-pile -> OnHostConvert ToPile the same tick as the spawn; death-watch retired)");
        }
        // A null function means GameplayStatics is not loaded yet; retried on the next Install.
    }

    // The InpActEvt_use interceptor family (the client-grab bridge, the use-deny suppressors, the
    // hard-throw bridge) lives in trash_use_intercept, which caches its own session and retries
    // until mainPlayer_C is loaded.
    coop::trash_use_intercept::Install(session);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    coop::trash_use_intercept::OnDisconnect();  // clears its cached session + gesture-pairing latch
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
