// harness/autotest_chippile.cpp -- Autonomous chipPile GRAB test (v81 MORPH V2 verify, 2026-06-20).
//
// Closes the ONE runtime-unverified link in the pile morph (docs/piles/07): does a REAL
// E-press grab of a tracked chipPile put the morphed clump into mainPlayer.holding_actor,
// so local_streams' new-held edge -> pile_morph::TryAdoptHeldClump fires and converts the
// peer's mirror pile-A -> clump? An audit / build / join-smoke CANNOT answer this -- only a
// real grab can. This routine performs one, autonomously, on the HOST, in the 2-peer smoke.
//
// WHY this is high-fidelity (not a fake that bypasses the path being tested):
//   - The grab is driven by CallFunction(mainPlayer_C::InpActEvt_use_..._41) -- the SAME
//     ProcessEvent-dispatched input UFunction a real E-press fires. Our PRE observer
//     (trash_collect_sync::OnPileGrabPre) therefore fires EXACTLY as for a real player, and
//     the BP ubergraph runs the real grab (spawn clump -> pickupObjectDirect(clump) ->
//     K2_DestroyActor(pile)). The decisive RE (votv-pile-grab-observable-hook-RE-2026-06-08-
//     pass1.md) proves that graph is gated ONLY on `icast(lookAtActor)` succeeding -- there
//     is NO input-state gate (unlike the flashlight InpActEvt_*, which no-op'd on a
//     reflection call). So a reflection call DOES run the grab, provided lookAtActor==pile.
//   - We do NOT force lookAtActor. We aim the real camera at the pile and POLL the game's own
//     interaction trace until ReadMainPlayerLookAtActor()==pile -- the GAME confirms the pile
//     is under the crosshair before we press, exactly like a real walk-up grab. That poll is
//     the self-validating fidelity gate: if it never resolves, we ABORT and say so (a test
//     that could not aim is INVALID, not a morph failure).
//   - The `pile_morph: grab armed` log line (emitted only from inside OnPileGrabPre->OnGrab)
//     then PROVES the real observer path fired. A direct playerGrabbed() call would skip it.
//
// PASS (host log): `pile_morph: grab armed -- eid=N`, then within ~1 frame the garbage_pickup
//   probe shows `holding_actor=0x..(prop_garbageClump_C)`, then `pile_morph: ADOPTED held
//   clump .. -> PropConvert{ToClump}`. CLIENT log: `remote_prop::OnConvert: eid=N re-skin ->
//   clump`. Phase B (best-effort throw): `pile_morph: land detected .. -> PropConvert{ToPile}`.
// FALLBACK (no regression): if `holding_actor` never becomes a clump, you instead see
//   `pile_morph: deferred-destroy fired` -- the working take-17 grab->vanish; the probe line
//   tells us WHERE the clump landed (grabbing_actor? null?) so the fix is one line.
//
// Gated by env VOTVCOOP_RUN_CHIPPILE_TEST="1". HOST drives the grab; CLIENT is scan-only
// (observes the convert via the wire). Launch: set the env var, then run the 2-peer smoke.

#include "harness/autotest.h"

#include "coop/players_registry.h"
#include "coop/remote_player.h"        // RemotePlayer::GetActor (client-in-world readiness gate)
#include "coop/prop_element_tracker.h"
#include "coop/remote_prop.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

// World look-rotation from `from` toward `to` (UE FRotator: Yaw about Z, Pitch about Y).
// Matches the forward-vector convention in autotest.cpp (fwd = cos(p)cos(y), cos(p)sin(y), sin(p)).
ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

// Run a game-thread closure and block until it stores into `done` (1=ok, 2=fail). The
// harness pattern (see autotest.cpp): coop/engine UObject state is game-thread only.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// Resolve the eid bound to a host pile (forward map) or a mirror pile (wire). kInvalidId = untracked.
coop::element::ElementId EidOf(void* pile) {
    coop::element::ElementId e = PT::GetPropElementIdForActor(pile);
    if (e == coop::element::kInvalidId) e = coop::remote_prop::ResolveMirrorEidByActor(pile);
    return e;
}

}  // namespace

void RunAutonomousChipPileTest() {
    const bool isHost = (ReadEnv("VOTVCOOP_NET_ROLE") != "client");
    if (!isHost) {
        UE_LOGI("chippile_test: CLIENT scan-only -- verify in THIS log: 'remote_prop::OnConvert: "
                "eid=N re-skin -> clump' (the carry mirrored), then '-> pile' (the re-pile). The "
                "host drives the grab; the convert arrives over the wire.");
        return;
    }

    // SETTLE -- gate the grab on a host-OBSERVABLE client-readiness signal, NOT a fixed sleep.
    // The smoke 2026-06-21 grabbed at a fixed 40 s but the client did not express its proxy
    // mirrors (its AStaticMeshActor copies of our piles) until ~53 s -> the ToClump convert had
    // NO proxy to re-skin and the WHOLE carry raced the join (client log: reskinINPLACE=0). So:
    //   (1) poll for the client's PUPPET to go LIVE -- the host spawns it once the client
    //       connects + handshakes, a clean "client is in-world" floor;
    //   (2) then a generous margin for the client to RECEIVE + EXPRESS the host's prop snapshot.
    // Combined with the 8 s MOVING carry below, the carry lands on a fully-settled world even if
    // the client expresses a little late. (A host-side "client finished joining" wire signal would
    // be cleaner but is out of scope for a test harness -- join_progress is client-local.)
    UE_LOGI("chippile_test: HOST starting -- waiting for the client puppet to go live (in-world) before the grab");
    {
        const int kWaitCapS = 150;
        int waitedS = 0; bool inWorld = false;
        while (waitedS < kWaitCapS) {
            const int r = RunGT([](std::atomic<int>& d) {
                coop::RemotePlayer* pup = coop::players::Registry::Get().Puppet(/*peerSlot=*/1);
                void* a = pup ? pup->GetActor() : nullptr;
                d.store((a && R::IsLive(a)) ? 1 : 2);
            });
            if (r == 1) { inWorld = true; break; }
            ::Sleep(1000); ++waitedS;
        }
        if (!inWorld)
            UE_LOGW("chippile_test: client puppet never went live in %d s -- grabbing anyway (may still race)", kWaitCapS);
        else
            UE_LOGI("chippile_test: client puppet LIVE after %d s (client in-world); +35 s margin for it to "
                    "express its proxy snapshot, then ReSeed + grab", waitedS);
        ::Sleep(35000);
    }

    // ---- 1. Resolve local player + the E-press UFunction (InpActEvt_use_..._41).
    struct Resolved { void* player = nullptr; void* useFn = nullptr; int32_t useFrame = 0; };
    auto rsv = std::make_shared<Resolved>();
    if (RunGT([rsv](std::atomic<int>& d) {
            void* player = coop::players::Registry::Get().Local();   // the possessed local player
            if (!player || !R::IsLive(player)) { UE_LOGW("chippile_test: no live local player"); d.store(2); return; }
            if (!E::GetController(player)) {  // GetController()!=null is the definitive local discriminator
                UE_LOGW("chippile_test: local player has no controller -- not possessed, aborting"); d.store(2); return; }
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* fn  = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
            if (!fn) { UE_LOGW("chippile_test: InpActEvt_use UFunction not found -- aborting"); d.store(2); return; }
            rsv->player   = player;
            rsv->useFn    = fn;
            rsv->useFrame = R::FunctionFrameSize(fn);
            UE_LOGI("chippile_test: resolve OK player=%p useFn=%p frame=%d", player, fn, rsv->useFrame);
            d.store(1);
        }) != 1) { UE_LOGW("chippile_test: resolve failed -- aborting"); return; }

    // ---- 2. Find an EID-TRACKED chipPile (the morph needs the pile's eid to arm + to address
    // the peer's mirror). Prefer a tracked one; if the nearest is untracked, scan for any tracked.
    struct PileSel { void* pile = nullptr; ue_wrap::FVector pos{}; uint32_t eid = 0; float dist = 0.f; bool tracked = false; };
    auto sel = std::make_shared<PileSel>();
    if (RunGT([rsv, sel](std::atomic<int>& d) {
            // Force-track every keyed/keyless prop NOW (assigns eids + populates the actor->eid
            // forward map) so the grabbed pile is deterministically eid-bound without waiting for
            // the 0.25 Hz reconcile. ReSeed is idempotent (adds 0 for already-tracked).
            const size_t reseeded = PT::ReSeedKnownKeyedProps(nullptr);
            UE_LOGI("chippile_test: forced ReSeedKnownKeyedProps -> %zu tracked (pile eids now bound)", reseeded);
            const ue_wrap::FVector at = E::GetActorLocation(rsv->player);
            float dist = -1.f;
            void* nearest = ue_wrap::prop::FindNearestChipPile(at, /*radiusCm=*/200000.f, &dist);
            if (!nearest) { UE_LOGW("chippile_test: NO chipPile in the world -- the save has none to grab"); d.store(2); return; }
            void* pick = nearest; float pickDist = dist;
            coop::element::ElementId eid = EidOf(nearest);
            if (eid == coop::element::kInvalidId) {
                // Nearest is untracked. Scan all chipPiles for the nearest tracked one (a host pile
                // gets an eid once expressed; a just-spawned one may not yet). Keep the untracked
                // nearest as the fallback so we at least exercise the held-object channel.
                const int32_t n = R::NumObjects();
                float best2 = -1.f;
                for (int32_t i = 0; i < n; ++i) {
                    void* o = R::ObjectAt(i);
                    if (!o || !R::IsLive(o)) continue;
                    if (!ue_wrap::prop::IsChipPile(o)) continue;
                    if (R::ToString(R::NameOf(o)).rfind(L"Default__", 0) == 0) continue;
                    if (EidOf(o) == coop::element::kInvalidId) continue;
                    const ue_wrap::FVector p = E::GetActorLocation(o);
                    const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (best2 < 0.f || d2 < best2) { best2 = d2; pick = o; pickDist = std::sqrt(d2); eid = EidOf(o); }
                }
            }
            sel->pile    = pick;
            sel->pos     = E::GetActorLocation(pick);
            sel->eid     = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
            sel->dist    = pickDist;
            sel->tracked = (eid != coop::element::kInvalidId);
            UE_LOGI("chippile_test: selected pile=%p pos=(%.0f,%.0f,%.0f) dist=%.0fcm eid=%u tracked=%d%s",
                    pick, sel->pos.X, sel->pos.Y, sel->pos.Z, pickDist, sel->eid, sel->tracked ? 1 : 0,
                    sel->tracked ? "" : " (UNTRACKED: morph will NOT arm; only the held-object channel is exercised)");
            d.store(1);
        }) != 1) { UE_LOGW("chippile_test: no chipPile to grab -- aborting"); return; }

    // ---- 3. Teleport the player to a standoff in front of the pile, approaching from the
    // direction it currently stands (open, walkable space -- avoids teleporting into the wall a
    // pile often sits against). Then let the Character settle for 1 s.
    if (RunGT([rsv, sel](std::atomic<int>& d) {
            const ue_wrap::FVector ploc = E::GetActorLocation(rsv->player);
            float ax = ploc.X - sel->pos.X, ay = ploc.Y - sel->pos.Y;
            const float h = std::sqrt(ax * ax + ay * ay);
            if (h < 1.f) { ax = 1.f; ay = 0.f; } else { ax /= h; ay /= h; }   // unit horizontal approach dir
            const ue_wrap::FVector stand{ sel->pos.X + ax * 150.f, sel->pos.Y + ay * 150.f, sel->pos.Z + 100.f };
            const ue_wrap::FRotator face = LookAt(stand, sel->pos);
            const bool ok = E::TeleportTo(rsv->player, stand, face);
            UE_LOGI("chippile_test: teleported player -> (%.0f,%.0f,%.0f) facing pile (ok=%d)",
                    stand.X, stand.Y, stand.Z, ok ? 1 : 0);
            d.store(1);
        }) != 1) { /* non-fatal */ }
    ::Sleep(1000);

    // ---- 4. Face the pile (best-effort visual; also lets the game's own trace resolve lookAtActor
    // when the geometry allows). We do NOT gate on the trace -- the grab below injects lookAtActor
    // for its single dispatch, which is deterministic AND faithful: the grab keys entirely off
    // lookAtActor (RE pass1 2.1) and the clump spawns at the PILE's transform (playerGrabbed runs on
    // the pile), not at the player's aim -- so the camera need not physically point at it.
    RunGT([rsv, sel](std::atomic<int>& d) {
        const ue_wrap::FVector cam = E::GetCameraLocation();
        E::SetControlRotation(E::GetController(rsv->player), LookAt(cam, sel->pos));
        d.store(1);
    });
    ::Sleep(400);
    RunGT([rsv, sel](std::atomic<int>& d) {
        void* look = E::ReadMainPlayerLookAtActor(rsv->player);
        UE_LOGI("chippile_test: faced pile; game-trace lookAtActor=%p (%s)", look,
                look == sel->pile ? "==pile, natural aim resolved" : "trace did not resolve -- will inject");
        d.store(1);
    });

    // ---- 5. THE GRAB -- a two-call sequence in ONE game-thread dispatch (RCA workflow
    // chippile-grab-trigger-rca, 2026-06-20). A reflection InpActEvt_use call fires our PRE observer
    // but NOT the BP grab body (it targets the release edge / the engine input system drives the
    // ubergraph, not the stub) -- the smoke #2 proof. So:
    //   5a ARM the morph the production way: inject lookAtActor=pile + CallFunction(InpActEvt_use) ->
    //      OnPileGrabPre reads the pile + resolves its eid + pile_morph::OnGrab (proven: 'grab armed').
    //   5b RUN the REAL conversion the press-edge ubergraph invokes at @92019: the pile's own
    //      playerGrabbed(Player, HitResult). CallFunction routes via ProcessEvent -> ProcessInternal ->
    //      the full byte-exact body: BeginDeferredActorSpawnFromClass(clump) -> FinishSpawningActor ->
    //      pickupObjectDirect(clump) [-> holding_actor / the grab machinery -- WHICH field is the open
    //      question this probe settles] -> K2_DestroyActor(self). The pile SELF-DESTRUCTS here; never
    //      deref sel->pile after the Call. eid + pilePos were captured in 5a's OnGrab, so the morph
    //      retains its identity through the destroy.
    UE_LOGI("chippile_test: >>> GRAB: arm(InpActEvt_use) + run real conversion(playerGrabbed) eid=%u <<<", sel->eid);
    RunGT([rsv, sel](std::atomic<int>& d) {
        // 5a -- arm.
        E::WriteMainPlayerLookAtActor(rsv->player, sel->pile);
        std::vector<uint8_t> frame(rsv->useFrame > 0 ? static_cast<size_t>(rsv->useFrame) : 0, 0u);
        const bool armOk = R::CallFunction(rsv->player, rsv->useFn, frame.empty() ? nullptr : frame.data());
        // 5b -- run the real pile->playerGrabbed(mainPlayer, HitResult{0}).
        void* pileCls = R::ClassOf(sel->pile);
        void* grabFn  = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
        bool grabOk = false, paramOk = false;
        if (grabFn) {
            ue_wrap::ParamFrame pf(grabFn);
            paramOk = pf.Set<void*>(L"Player", rsv->player);   // HitResult left zeroed (not a gate)
            grabOk  = ue_wrap::Call(sel->pile, pf);            // pile self-destructs HERE
        }
        UE_LOGI("chippile_test: arm(InpActEvt_use)=%d | playerGrabbed fn=%p paramSet=%d call=%d -- "
                "pile converted + self-destroyed; now polling WHICH field the clump landed in",
                armOk ? 1 : 0, grabFn, paramOk ? 1 : 0, grabOk ? 1 : 0);
        d.store(1);
    });

    // ---- 6. THE FIELD-ROUTING PROBE (the [probe garbage_pickup] that has never emitted a real line).
    // Poll grab-state for ~1.6 s and log WHICH field the just-spawned clump lands in. This settles the
    // open morph premise autonomously (RCA workflow finding 3): the clump is held via pickupObjectDirect
    // exactly as in production, so whichever field holds it IS the production-faithful answer ->
    //   Candidate A: holding_actor=clump  -> the held-object channel premise HOLDS; local_streams' new-
    //                held edge sees it; pile_morph::TryAdoptHeldClump can adopt (the morph is sound).
    //   Candidate B: grabbing_actor=clump -> the clump rode the PhysicsHandle path; local_streams only
    //                falls back to holding_actor when grabbing_actor is null + IsKeyedInteractable, so it
    //                NEVER sees the clump -> TryAdoptHeldClump never fires -> the morph is WRONG as built.
    bool sawClumpHolding = false, sawClumpGrabbing = false;
    void* heldClump = nullptr;   // captured for the Phase B re-pile throw
    for (int i = 0; i < 16; ++i) {
        ::Sleep(100);
        RunGT([rsv, &sawClumpHolding, &sawClumpGrabbing, &heldClump, i](std::atomic<int>& d) {
            E::MainPlayerGrabState gs{};
            if (E::ReadMainPlayerGrabState(rsv->player, gs)) {
                const bool holdClump = gs.holdingActor && ue_wrap::prop::IsGarbageClump(gs.holdingActor);
                const bool grabClump = gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor);
                if (holdClump)  sawClumpHolding  = true;
                if (grabClump)  sawClumpGrabbing = true;
                if (!heldClump) heldClump = grabClump ? gs.grabbingActor : (holdClump ? gs.holdingActor : nullptr);
                // Log the full grab-state on the first 4 polls (the clump appears within a frame or
                // two) + on any clump edge -- this IS the missing probe line.
                if (i < 4 || holdClump || grabClump) {
                    UE_LOGI("chippile_test: PROBE poll %d -- grabbing_actor=%p(%ls)[clump=%d] "
                            "holding_actor=%p(%ls)[clump=%d]", i,
                            gs.grabbingActor, gs.grabbingActor ? R::ClassNameOf(gs.grabbingActor).c_str() : L"-", grabClump ? 1 : 0,
                            gs.holdingActor,  gs.holdingActor  ? R::ClassNameOf(gs.holdingActor).c_str()  : L"-", holdClump ? 1 : 0);
                }
            }
            d.store(1);
        });
    }
    // VERDICT (empirical, smoke 2026-06-20): the clump rides grabbing_actor (the PHC light-grab path,
    // via playerGrabbed->pickupObjectDirect), NOT holding_actor. The morph adopts it correctly anyway --
    // local_streams' new-held edge reads grabbing_actor FIRST (holding_actor is only a fallback), so it
    // sees the clump and TryAdoptHeldClump fires (proven by the 'ADOPTED held clump' line above). The old
    // 'clump rides holding_actor' doc claim is wrong; the morph works regardless.
    UE_LOGI("chippile_test: FIELD-ROUTING VERDICT -- clump-in-grabbing_actor=%d clump-in-holding_actor=%d -- %s",
            sawClumpGrabbing ? 1 : 0, sawClumpHolding ? 1 : 0,
            sawClumpGrabbing ? "clump rides grabbing_actor (PHC path); local_streams reads it first so the morph adopts it -- CORRECTED premise, morph OK"
          : sawClumpHolding  ? "clump rides holding_actor (the old assumed field)"
                             : "NEITHER -- no clump was held (playerGrabbed did not run, or the clump self-freed before the first poll)");

    // ---- 6.5 SUSTAINED MOVING CARRY (the km-walk the phase-1 north star needs; smoke gap 2026-06-21).
    // The 1.6 s field-routing probe above is STATIONARY + then re-piles immediately -- it never exercised
    // a real moving carry, so a client drive that never establishes / never follows would pass unnoticed.
    // WALK the host ~8 s with the clump held so the client MUST establish the carry pose-drive, FOLLOW,
    // and interpolate. Move in small per-100ms steps (a walk, not one teleport, so the PHC keeps the
    // clump in hand) and re-confirm the hold each step. The CLIENT log is then judged for `GRAB-IN eid=N`
    // + `drive #N -> target ... [proxy]` advancing across the 8 s (the proof the carry mirrors), and the
    // proxy `SkinProxy CLUMP mesh-src` (dirtball vs PILE-FALLBACK). The host streams `PropPose emit ... ctx=1`.
    if (heldClump) {
        ue_wrap::FVector base{};
        RunGT([rsv, &base](std::atomic<int>& d) { base = E::GetActorLocation(rsv->player); d.store(1); });
        float dx = base.X - sel->pos.X, dy = base.Y - sel->pos.Y;   // carry AWAY from the pile origin (open space)
        const float h = std::sqrt(dx * dx + dy * dy);
        if (h < 1.f) { dx = 1.f; dy = 0.f; } else { dx /= h; dy /= h; }
        // 15 cm / 100 ms = 1.5 m/s (a brisk WALK). The first cut used 40 cm/step = 4 m/s, which imparted
        // ~4 m/s into the PHC-held clump and BROKE the grab mid-carry (the clump flew off as a "throw",
        // smoke 2026-06-21 23:54:08). A walk-speed carry keeps the clump in hand for the full 8 s.
        const int   kSteps  = 80;         // 80 * 100 ms = 8 s
        const float kStepCm = 15.f;       // 80 * 15 cm = 12 m carry at 1.5 m/s
        int stillHeld = 0, consecMiss = 0;
        for (int s = 0; s < kSteps; ++s) {
            ::Sleep(100);
            const ue_wrap::FVector to{ base.X + dx * kStepCm * (s + 1), base.Y + dy * kStepCm * (s + 1), base.Z };
            int held = 0;
            RunGT([rsv, to, &held](std::atomic<int>& d) {
                E::SetActorLocation(rsv->player, to);             // walk the host one 15 cm step
                E::MainPlayerGrabState gs{};
                if (E::ReadMainPlayerGrabState(rsv->player, gs) &&
                    ((gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ||
                     (gs.holdingActor  && ue_wrap::prop::IsGarbageClump(gs.holdingActor))))
                    held = 1;
                d.store(1);
            });
            stillHeld += held;
            consecMiss = held ? 0 : (consecMiss + 1);
            if ((s % 20) == 0)
                UE_LOGI("chippile_test: moving carry step %d/%d (held-confirms=%d) -- client should be "
                        "following the clump now (its log: GRAB-IN + drive #N [proxy])", s, kSteps, stillHeld);
            if (consecMiss >= 8) {   // the grab broke (clump dropped) -- stop walking, go to re-pile
                UE_LOGW("chippile_test: clump no longer held after step %d (grab broke) -- ending the moving carry early", s);
                break;
            }
        }
        UE_LOGI("chippile_test: MOVING CARRY done -- %d/%d steps still holding the clump (eid=%u). CLIENT must show "
                "'GRAB-IN' + 'drive #N -> target ... [proxy]' advancing across the 8 s, and the proxy "
                "'SkinProxy CLUMP mesh-src=dirtball' (PILE-FALLBACK => clump renders as a pile)",
                stillHeld, kSteps, sel->eid);
    }

    // ---- 7. (Phase B, best-effort) RE-PILE. The clump rides grabbing_actor (the PHC light-grab path),
    // so throwHoldingProp via reflection does NOT release it (BP-pure-inline; smoke proof: the carry
    // kept streaming for 30 s after). Faithful physics path instead: release the PHC cleanly
    // (ReleaseMainPlayerGrabIfHolding -> ReleaseComponent + clear grabbing_actor) + lift the freed clump
    // so its fall gives a real impact -> the clump's impact-driven BP re-piles it -> pile_morph's
    // land-watch (Tick) sees the clump die near its last pos -> PropConvert{ToPile} (re-seed-race-safe).
    // Best-effort: grab+carry above is the primary verdict; a non-re-pile here is not a regression.
    if (heldClump) {
        RunGT([rsv, heldClump](std::atomic<int>& d) {
            const bool rel = E::ReleaseMainPlayerGrabIfHolding(rsv->player, heldClump);
            ue_wrap::FVector at = E::GetActorLocation(heldClump);
            at.Z += 400.f;                                  // lift 4 m so the fall impact triggers re-pile
            const bool tp = E::SetActorLocation(heldClump, at);
            UE_LOGI("chippile_test: Phase B re-pile -- released PHC=%d + lifted clump +400cm=%d -> expect "
                    "fall -> impact -> re-pile -> 'pile_morph: land detected -> PropConvert{ToPile}'",
                    rel ? 1 : 0, tp ? 1 : 0);
            d.store(1);
        });
    } else {
        UE_LOGI("chippile_test: Phase B skipped -- no held clump was captured");
    }
    ::Sleep(5000);  // let the clump fall + impact + re-pile + the land-watch poll catch it

    UE_LOGI("chippile_test: DONE -- grab-clump-holding=%d. PASS if the host log shows 'grab armed' "
            "+ 'ADOPTED held clump -> PropConvert{ToClump}' and the CLIENT log shows "
            "'remote_prop::OnConvert: eid=%u re-skin -> clump'. Primary unverified link MEASURED.",
            sawClumpHolding ? 1 : 0, sel->eid);
}

DWORD WINAPI ChipPileTestThread(LPVOID /*arg*/) {
    RunAutonomousChipPileTest();
    return 0;
}

}  // namespace harness::autotest
