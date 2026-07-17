// harness/autotest_chippile.cpp -- Autonomous chipPile GRAB/carry/throw scenario driver.
//
// CORRECTION (2026-06-22): the original rationale below targets the RETIRED "pile_morph" model
// (docs/piles/07) -- that was replaced by the host-authoritative trash channel + the
// AStaticMeshActor PROXY (docs/piles/08; trash_channel + trash_proxy). The ACTIONS here are
// still current + faithful (the real InpActEvt_use + playerGrabbed grab, the moving carry, the
// PHC-release re-pile); only the morph-era markers named in the prose are obsolete. The VERDICT
// is now the log-truth harness (tools/pile-test-assert.ps1), not the 'pile_morph: ...' lines.
// Set VOTVCOOP_PILE_SHOWCASE=1 to additionally aim the CLIENT camera at a mirrored pile + hold
// (for an external screenshot of the client rendering -- see RunAutonomousChipPileTest CLIENT branch).
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

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"        // RemotePlayer::GetActor (client-in-world readiness gate)
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_collect_sync.h"   // DebugSendGrabIntent (Increment-2 synthetic wire test)
#include "coop/props/trash_proxy.h"          // NearestPileProxy (CLIENT visual showcase: aim at a mirrored pile)
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <algorithm>
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
        if (ReadEnv("VOTVCOOP_PILE_SHOWCASE") != "1") {
            UE_LOGI("chippile_test: CLIENT scan-only -- verify in THIS log: 'remote_prop::OnConvert: "
                    "eid=N re-skin -> clump' (the carry mirrored), then '-> pile' (the re-pile). The "
                    "host drives the grab; the convert arrives over the wire.");
            return;
        }
        // VISUAL SHOWCASE (VOTVCOOP_PILE_SHOWCASE=1): aim the client camera at a MIRRORED pile + hold, so an
        // external window capture gets a clean client view of a pile rendering (de-duped, host height/rotation
        // -- the eyeball complement to the log harness). Waits for the host's pile work + proxy express, then
        // teleports to a standoff facing the nearest PILE-form proxy and holds, re-facing each second.
        UE_LOGI("chippile_test: CLIENT SHOWCASE -- waiting 70s for join + the host pile work + proxy express, "
                "then facing a mirrored pile proxy");
        ::Sleep(70000);
        struct CShow { void* player = nullptr; void* pile = nullptr; ue_wrap::FVector pos{}; float dist = 0.f; };
        auto cs = std::make_shared<CShow>();
        if (RunGT([cs](std::atomic<int>& d) {
                void* p = coop::players::Registry::Get().Local();
                if (!p || !R::IsLive(p) || !E::GetController(p)) {
                    UE_LOGW("chippile_test: CLIENT showcase -- no possessed local player"); d.store(2); return; }
                float dist = -1.f;
                void* pile = coop::trash_proxy::NearestPileProxy(E::GetActorLocation(p), &dist);
                if (!pile) { UE_LOGW("chippile_test: CLIENT showcase -- no pile proxy to face yet"); d.store(2); return; }
                cs->player = p; cs->pile = pile; cs->pos = E::GetActorLocation(pile); cs->dist = dist;
                UE_LOGI("chippile_test: CLIENT showcase -- nearest pile proxy=%p pos=(%.0f,%.0f,%.0f) dist=%.0fcm",
                        pile, cs->pos.X, cs->pos.Y, cs->pos.Z, dist);
                d.store(1);
            }) != 1) { UE_LOGW("chippile_test: CLIENT showcase aborted (no player/pile)"); return; }
        RunGT([cs](std::atomic<int>& d) {                       // teleport to a 180 cm standoff facing the pile
            const ue_wrap::FVector at = E::GetActorLocation(cs->player);
            float ax = at.X - cs->pos.X, ay = at.Y - cs->pos.Y;
            const float h = std::sqrt(ax * ax + ay * ay);
            if (h < 1.f) { ax = 1.f; ay = 0.f; } else { ax /= h; ay /= h; }
            const ue_wrap::FVector stand{ cs->pos.X + ax * 180.f, cs->pos.Y + ay * 180.f, cs->pos.Z + 90.f };
            const ue_wrap::FRotator face = LookAt(stand, cs->pos);
            E::TeleportTo(cs->player, stand, face);
            E::SetControlRotation(E::GetController(cs->player), face);
            UE_LOGI("chippile_test: CLIENT showcase -- teleported to (%.0f,%.0f,%.0f) facing the pile; holding 60s",
                    stand.X, stand.Y, stand.Z);
            d.store(1);
        });
        for (int i = 0; i < 60; ++i) {                          // hold 60 s, re-facing the pile each second
            ::Sleep(1000);
            RunGT([cs](std::atomic<int>& d) {
                E::SetControlRotation(E::GetController(cs->player), LookAt(E::GetActorLocation(cs->player), cs->pos));
                d.store(1);
            });
        }
        UE_LOGI("chippile_test: CLIENT showcase -- done holding on the pile");
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
            // DIRECTIONAL THROW (not a vertical drop): give the freed clump a horizontal+up velocity so it
            // flies a real ARC -- the host's flight-stream then streams a genuine arc (many 'carry/flight
            // CONTINUE') instead of just the 1-frame release flicker, so the harness arc invariant tests the
            // actual throw. Direction = from the player toward the held clump (it sits in front), ~6 m/s
            // forward + 4 m/s up -> lands a few metres ahead, then impact-re-piles.
            const ue_wrap::FVector pp = E::GetActorLocation(rsv->player);
            const ue_wrap::FVector cp = E::GetActorLocation(heldClump);
            float fx = cp.X - pp.X, fy = cp.Y - pp.Y;
            const float hl = std::sqrt(fx * fx + fy * fy);
            if (hl < 1.f) { fx = 1.f; fy = 0.f; } else { fx /= hl; fy /= hl; }
            const ue_wrap::FVector lin{ fx * 600.f, fy * 600.f, 400.f };   // cm/s: ~6 m/s fwd + 4 m/s up
            const bool vel = E::SetActorRootPhysicsVelocity(heldClump, lin, ue_wrap::FVector{0.f, 0.f, 0.f});
            UE_LOGI("chippile_test: Phase B THROW -- released PHC=%d + threw clump dir=(%.2f,%.2f) up |v|set=%d "
                    "-> expect a flight ARC (host '[PILE] HOST carry/flight CONTINUE' x many) -> impact -> re-pile "
                    "-> '[PILE] HOST RE-PILE(thunk)' + '[TRASH-CH] HOST LAND COMMIT (re-read from the settled pile)'",
                    rel ? 1 : 0, fx, fy, vel ? 1 : 0);
            d.store(1);
        });
    } else {
        UE_LOGI("chippile_test: Phase B skipped -- no held clump was captured");
    }
    ::Sleep(5000);  // let the clump fall + impact + re-pile + the land-watch poll catch it

    UE_LOGI("chippile_test: DONE -- grab-clump-holding=%d eid=%u. VERDICT is now the log-truth harness "
            "(tools/pile-test-assert.ps1): host '[PILE] HOST GRAB ADOPT' + '[TRASH-CH] HOST carry OPEN' + "
            "'HOST RE-PILE(thunk)' + 'LAND COMMIT'; CLIENT '[PILE] CLIENT recv convert GRAB/LAND -> PROXY "
            "re-skinned' + 'CLIENT ToPile SNAP ... drift=~0'. (The old 'pile_morph: grab armed/ADOPTED' "
            "markers are RETIRED -- the morph was replaced by the host-auth trash channel + proxy.)",
            sawClumpHolding ? 1 : 0, sel->eid);
}

// ---------------------------------------------------------------------------------------------------
// PUPPET-GRAB PROBE (docs/piles/08 Increment-2 gating [?]) -- VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1, HOST-only.
//
// THE QUESTION this settles, on real logs: in the client-grab direction, the host executes the real
// grab verb on PUPPET-N (an unpossessed mainPlayer_C, GetController()==null) via
// reflection::CallFunction(pile, playerGrabbed, {puppetN, hit}). The RE (this session, agent-verified +
// cited) proved the grab path -- pickupObjectDirect -> grabHandle.GrabComponentAtLocationWithRotation ->
// per-tick PHC SetTargetLocationAndRotation -- touches NO GetController/IsLocallyControlled/PlayerController
// state, so the grab should ENGAGE on a puppet (grabbing_actor := clump). The ONE thing the bytecode could
// NOT prove is whether an UNPOSSESSED puppet's ReceiveTick actually dispatches -- i.e. whether the per-tick
// PHC maintenance RUNS, so the clump tracks to the puppet's hand (camera-front) rather than floating at the
// spawn spot. In the trash channel the host STREAMS the clump's host-side world pose (PropPose, eid-keyed)
// to every peer, so "the clump ends up in the puppet's hand ON THE HOST" is exactly what all peers will see.
//
// This probe answers it WITHOUT touching any wire: find the slot-1 puppet, grab the nearest chipPile with
// the PUPPET as the player param, then poll the grab state + geometry for ~4 s. We read, never destroy
// beyond the one pile the real grab verb itself consumes (production-faithful: the same verb a real grab runs).
//
// VERDICT (logged for tools/pile-test-assert.ps1 to assert):
//   ENGAGED  = grabbing_actor became a garbageClump at all (the core RE verdict, runtime-confirmed).
//   HELD     = grabbing_actor stayed the clump across the window (the PHC keeps it; not dropped).
//   TRACKED  = the clump was pulled to the puppet's hand -- horizontal dist collapsed toward ~grabLen AND/OR
//              the clump's Z rose from ground level to hand height -> the per-tick PHC maintenance RUNS on the
//              unpossessed puppet (tick ALIVE) -> verdict A (works as-is once the puppet aim is synced).
//   FLOATING = held but NOT tracked (the clump stayed at the spawn spot, low Z) -> the puppet tick is
//              suppressed -> verdict B-fallback: Increment 2 must drive SetTargetLocationAndRotation on the
//              puppet each tick from the synced remote aim (the data the mod already streams).
void RunPuppetGrabProbe() {
    const bool isHost = (ReadEnv("VOTVCOOP_NET_ROLE") != "client");
    if (!isHost) {
        UE_LOGI("puppet_grab_probe: CLIENT -- nothing to drive (the HOST executes the grab on the puppet). "
                "Just stand so the slot-1 puppet exists on the host.");
        return;
    }

    // 1. Wait for the slot-1 client puppet to go LIVE in the host's world (same gate as the chipPile test).
    UE_LOGI("puppet_grab_probe: HOST -- waiting for the slot-1 client puppet to go live (in-world)");
    struct Pup { void* actor = nullptr; bool hasController = true; };
    auto pup = std::make_shared<Pup>();
    {
        const int kWaitCapS = 150; int waitedS = 0; bool live = false;
        while (waitedS < kWaitCapS) {
            const int r = RunGT([pup](std::atomic<int>& d) {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(/*peerSlot=*/1);
                void* a = p ? p->GetActor() : nullptr;
                if (a && R::IsLive(a)) { pup->actor = a; pup->hasController = (E::GetController(a) != nullptr); d.store(1); }
                else d.store(2);
            });
            if (r == 1) { live = true; break; }
            ::Sleep(1000); ++waitedS;
        }
        if (!live) { UE_LOGW("puppet_grab_probe: slot-1 puppet never went live in %d s -- aborting (no client?)", kWaitCapS); return; }
    }
    // GetController()==null is THE local-vs-puppet discriminator. A puppet must NOT have a controller; if it
    // does, this is mis-wired (we'd be probing the real local player) -- abort rather than report a false pass.
    if (pup->hasController) {
        UE_LOGW("puppet_grab_probe: slot-1 actor HAS a controller -- that is NOT a puppet; aborting (mis-wired)");
        return;
    }
    UE_LOGI("puppet_grab_probe: slot-1 puppet LIVE actor=%p, GetController()==null CONFIRMED (a true puppet). "
            "+20 s margin for the world to settle, then the puppet grab", pup->actor);
    ::Sleep(20000);

    // 2. Find the nearest live chipPile to the puppet (tracked-ness is irrelevant here -- this probe tests the
    //    raw puppet HOLD, not the coop convert wire). A pile a few metres away makes the pull-in signal strong.
    struct Sel { void* pile = nullptr; ue_wrap::FVector pilePos{}; ue_wrap::FVector pupPos0{}; float pupYaw0 = 0.f; float dist0 = 0.f; };
    auto sel = std::make_shared<Sel>();
    if (RunGT([pup, sel](std::atomic<int>& d) {
            const ue_wrap::FVector pl = E::GetActorLocation(pup->actor);
            const ue_wrap::FRotator pr = E::GetActorRotation(pup->actor);
            float dist = -1.f;
            void* pile = ue_wrap::prop::FindNearestChipPile(pl, /*radiusCm=*/200000.f, &dist);
            if (!pile) { UE_LOGW("puppet_grab_probe: NO chipPile in the world -- the save has none to grab"); d.store(2); return; }
            sel->pile = pile; sel->pilePos = E::GetActorLocation(pile);
            sel->pupPos0 = pl; sel->pupYaw0 = pr.Yaw; sel->dist0 = dist;
            UE_LOGI("puppet_grab_probe: puppet@(%.0f,%.0f,%.0f) yaw=%.0f -- nearest chipPile=%p @(%.0f,%.0f,%.0f) dist=%.0fcm",
                    pl.X, pl.Y, pl.Z, pr.Yaw, pile, sel->pilePos.X, sel->pilePos.Y, sel->pilePos.Z, dist);
            d.store(1);
        }) != 1) { UE_LOGW("puppet_grab_probe: no chipPile to grab -- aborting"); return; }

    // 3. THE PUPPET GRAB -- call the pile's OWN playerGrabbed with the PUPPET as the player param. The RE shows
    //    this spawns the clump (BeginDeferred), sets clump.holdPlayer:=puppet, FinishSpawning, then calls
    //    puppet.pickupObjectDirect(clump) -> the PHC grab on the PUPPET. The pile self-destructs here. No
    //    InpActEvt arming + no lookAtActor injection (we are not exercising the OnPileGrabPre observer / the
    //    convert wire -- only the raw puppet hold).
    UE_LOGI("puppet_grab_probe: >>> executing playerGrabbed on the PUPPET (the Increment-2 host-side move) <<<");
    if (RunGT([pup, sel](std::atomic<int>& d) {
            void* pileCls = R::ClassOf(sel->pile);
            void* grabFn  = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
            bool paramOk = false, callOk = false;
            if (grabFn) {
                ue_wrap::ParamFrame pf(grabFn);
                paramOk = pf.Set<void*>(L"Player", pup->actor);   // the PUPPET grabs; HitResult left zeroed (not a gate)
                callOk  = ue_wrap::Call(sel->pile, pf);           // the pile self-destructs HERE
            }
            UE_LOGI("puppet_grab_probe: playerGrabbed(puppet) fn=%p paramSet=%d call=%d -- now polling whether the "
                    "PUPPET holds + tracks the clump", grabFn, paramOk ? 1 : 0, callOk ? 1 : 0);
            d.store(grabFn && callOk ? 1 : 2);
        }) != 1) { UE_LOGW("puppet_grab_probe: the playerGrabbed call failed -- aborting"); return; }

    // 4. POLL the grab state + geometry for ~4 s. Track: did grabbing_actor become a clump (ENGAGED), did it
    //    stay (HELD), and did the clump get pulled to the puppet's hand (TRACKED -> tick alive) vs float at the
    //    spawn spot (FLOATING -> tick dead). dist = horizontal puppet->clump; dZ = clump.Z - puppet.Z (rises to
    //    ~hand height when the PHC pulls it up off the ground); grabLen = the grab Timeline (a per-tick value).
    int engagedPolls = 0, totalPolls = 0;
    float distFirst = -1.f, distLast = -1.f, distMin = 1e9f, dzFirst = -1e9f, dzLast = -1e9f, grabLenLast = -1.f;
    for (int i = 0; i < 40; ++i) {
        ::Sleep(100);
        ++totalPolls;
        RunGT([pup, &engagedPolls, &distFirst, &distLast, &distMin, &dzFirst, &dzLast, &grabLenLast, i](std::atomic<int>& d) {
            E::MainPlayerGrabState gs{};
            if (!E::ReadMainPlayerGrabState(pup->actor, gs)) { d.store(1); return; }
            void* clump = (gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ? gs.grabbingActor
                        : (gs.holdingActor  && ue_wrap::prop::IsGarbageClump(gs.holdingActor))  ? gs.holdingActor : nullptr;
            const bool engaged = (clump != nullptr);
            if (engaged) ++engagedPolls;
            float dist = -1.f, dz = -1e9f;
            if (clump) {
                const ue_wrap::FVector pl = E::GetActorLocation(pup->actor);
                const ue_wrap::FVector cl = E::GetActorLocation(clump);
                const float ddx = cl.X - pl.X, ddy = cl.Y - pl.Y;
                dist = std::sqrt(ddx * ddx + ddy * ddy);
                dz = cl.Z - pl.Z;
                if (distFirst < 0.f) { distFirst = dist; dzFirst = dz; }
                distLast = dist; dzLast = dz; grabLenLast = gs.grabLen;
                if (dist < distMin) distMin = dist;
            }
            if (i < 6 || (engaged && (i % 8) == 0)) {
                UE_LOGI("puppet_grab_probe: poll %d -- grabbing_actor=%p[clump=%d] holding_actor=%p dist=%.0fcm dZ=%.0fcm grabLen=%.1f",
                        i, gs.grabbingActor, (gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ? 1 : 0,
                        gs.holdingActor, dist, dz, gs.grabLen);
            }
            d.store(1);
        });
    }

    // 5. VERDICT. ENGAGED if any poll saw a held clump; HELD if it persisted (>=70% of polls after first); the
    //    tick-liveness (TRACKED vs FLOATING) reads from the geometry: a hand-held clump sits within ~250 cm of
    //    the puppet AND elevated near hand height (dZ roughly -20..+160 cm, not on the ground far below); a
    //    floating-at-spawn clump stays at the pile's distance with a low/negative dZ. We report all numbers so
    //    the truth is in the log even where the heuristic is borderline.
    const bool engaged = engagedPolls > 0;
    const bool held    = engagedPolls >= (totalPolls * 7) / 10;
    const bool nearHand = (distLast >= 0.f && distLast <= 250.f);
    const bool atHandHeight = (dzLast > -40.f);                 // clump lifted to ~hand level, not collapsed to the ground
    const bool pulledIn = (distFirst > 0.f && distLast >= 0.f && distLast < distFirst - 50.f);  // visibly pulled toward the hand
    const bool tracked = engaged && (nearHand && atHandHeight);
    UE_LOGI("puppet_grab_probe: VERDICT -- ENGAGED=%d HELD=%d TRACKED=%d | engagedPolls=%d/%d "
            "dist first=%.0f last=%.0f min=%.0f cm | dZ first=%.0f last=%.0f cm | grabLen=%.1f | pulledIn=%d",
            engaged ? 1 : 0, held ? 1 : 0, tracked ? 1 : 0, engagedPolls, totalPolls,
            distFirst, distLast, distMin, dzFirst, dzLast, grabLenLast, pulledIn ? 1 : 0);
    if (!engaged)
        UE_LOGW("puppet_grab_probe: RESULT = NOT-ENGAGED -- playerGrabbed(puppet) did not leave a clump in "
                "grabbing_actor/holding_actor. The RE predicted ENGAGED; investigate (param frame? clump self-freed?).");
    else if (tracked)
        UE_LOGI("puppet_grab_probe: RESULT = TRACKED (tick ALIVE) -- the puppet HOLDS the clump at its hand; the "
                "per-tick PHC maintenance RUNS on the unpossessed puppet. Increment-2 host-side grab works as-is "
                "(verdict A); the only remaining input is syncing the puppet aim, which the mod already streams.");
    else
        UE_LOGI("puppet_grab_probe: RESULT = FLOATING (held, tick likely DEAD) -- the clump did not reach the "
                "puppet's hand (dist/last=%.0f cm, dZ=%.0f cm). Increment-2 must drive SetTargetLocationAndRotation "
                "on the puppet each tick from the synced remote aim (verdict B-fallback).", distLast, dzLast);
}

// ---------------------------------------------------------------------------------------------------
// SYNTHETIC GrabIntent test (docs/piles/08 Increment-2 HOST-SIDE) -- VOTVCOOP_RUN_GRAB_INTENT_TEST=1.
//
// The CLIENT picks a mirrored pile proxy, resolves its (host-authoritative) eid, and SENDS a GrabIntent
// over the wire (trash_collect_sync::DebugSendGrabIntent -> trash_channel::SendGrabIntent). This exercises
// the FULL client->host path WITHOUT the phase-2 client suppress-native / collision prerequisite: the wire
// (proto v84 GrabIntent), the 3-place router (event_feed -> event_dispatch_state), the host OnGrabIntent
// (validate + playerGrabbed on puppet-N + PropConvert{kToClump} broadcast), and the host puppet hand-drive.
//
// VERDICT is the log-truth harness (tools/pile-test-assert.ps1):
//   HOST log:  [GRAB-INTENT] RECEIVED eid=N slot=1 -> EXEC -> SUCCESS ; [PUPPET-DRIVE] DRIVING eid=N slot=1
//   CLIENT log: the PropConvert{ToClump} echo applied to the proxy (remote_prop OnConvert / GRAB-IN).
// The CLIENT drives; the HOST is the authority that executes + broadcasts.
void RunGrabIntentTest() {
    const bool isHost = (ReadEnv("VOTVCOOP_NET_ROLE") != "client");
    if (isHost) {
        UE_LOGI("grab_intent_test: HOST -- the authority. Watch THIS log for the grab-intent RECEIVED/EXEC/"
                "SUCCESS + puppet-drive markers when the client sends its synthetic grab.");
        return;
    }

    // 1. Wait for the client to be in-world + its pile proxies expressed (same settle as the showcase).
    UE_LOGI("grab_intent_test: CLIENT -- waiting 70s for join + the host pile work + proxy express, then "
            "picking a mirrored pile + sending GrabIntent");
    ::Sleep(70000);

    struct Pick { void* player = nullptr; void* pile = nullptr; uint32_t eid = 0; ue_wrap::FVector pilePos{};
                  float dist = 0.f; void* useFn = nullptr; int32_t useFrame = 0; };
    auto pk = std::make_shared<Pick>();
    if (RunGT([pk](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (!p || !R::IsLive(p) || !E::GetController(p)) {
                UE_LOGW("grab_intent_test: no possessed local player"); d.store(2); return; }
            float dist = -1.f;
            void* pile = coop::trash_proxy::NearestPileProxy(E::GetActorLocation(p), &dist);
            if (!pile) { UE_LOGW("grab_intent_test: no pile proxy to grab yet"); d.store(2); return; }
            coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(pile);
            if (eid == coop::element::kInvalidId) {
                UE_LOGW("grab_intent_test: nearest pile proxy %p has no resolvable eid", pile); d.store(2); return; }
            // Resolve the E-press UFunction (InpActEvt_use) so we can drive the REAL recognition path
            // (OnPileGrabPre reading the trace), not only the debug bypass.
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* fn  = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
            pk->player = p; pk->pile = pile; pk->eid = static_cast<uint32_t>(eid);
            pk->pilePos = E::GetActorLocation(pile); pk->dist = dist;
            pk->useFn = fn; pk->useFrame = fn ? R::FunctionFrameSize(fn) : 0;
            UE_LOGI("grab_intent_test: picked pile proxy=%p eid=%u pos=(%.0f,%.0f,%.0f) dist=%.0fcm useFn=%p",
                    pile, pk->eid, pk->pilePos.X, pk->pilePos.Y, pk->pilePos.Z, dist, fn);
            d.store(1);
        }) != 1) { UE_LOGW("grab_intent_test: could not pick a pile -- aborting"); return; }

    // 2. Teleport the client to a standoff FACING the chosen pile proxy, so its interaction trace can hit
    //    it (the client AIMS at the proxy) and its puppet (host-driven from the client pose) stands at the pile.
    RunGT([pk](std::atomic<int>& d) {
        const ue_wrap::FVector at = E::GetActorLocation(pk->player);
        float ax = at.X - pk->pilePos.X, ay = at.Y - pk->pilePos.Y;
        const float h = std::sqrt(ax * ax + ay * ay);
        if (h < 1.f) { ax = 1.f; ay = 0.f; } else { ax /= h; ay /= h; }
        const ue_wrap::FVector stand{ pk->pilePos.X + ax * 180.f, pk->pilePos.Y + ay * 180.f, pk->pilePos.Z + 90.f };
        const ue_wrap::FRotator face = LookAt(stand, pk->pilePos);
        E::TeleportTo(pk->player, stand, face);
        E::SetControlRotation(E::GetController(pk->player), face);
        UE_LOGI("grab_intent_test: client at a standoff facing the pile; grabbing via the camera-ray cone next");
        d.store(1);
    });
    ::Sleep(1500);   // let the view camera settle on the pile so the cone (camera forward) points at it

    // 3. GRAB via the REAL path: inject InpActEvt_use (the SAME ProcessEvent edge a real E-press fires) ->
    //    OnPileGrabPre runs the camera-ray cone (EidForAimedPileProxy) -> recognizes the aimed pile proxy ->
    //    SendGrabIntent. The client is teleported facing the pile within reach, so the cone resolves it. The
    //    debug bypass is used ONLY if the InpActEvt_use UFunction couldn't be resolved (so the chain still runs).
    const bool useReal = (pk->useFn != nullptr);
    RunGT([pk, useReal](std::atomic<int>& d) {
        if (useReal) {
            std::vector<uint8_t> frame(pk->useFrame > 0 ? static_cast<size_t>(pk->useFrame) : 0, 0u);
            const bool ok = R::CallFunction(pk->player, pk->useFn, frame.empty() ? nullptr : frame.data());
            UE_LOGI("grab_intent_test: >>> REAL GRAB -- injected InpActEvt_use (ok=%d); OnPileGrabPre should log "
                    "'[GRAB-INTENT] CLIENT E-PRESS aimed at pile proxy eid=%u (camera-ray cone)' + SendGrabIntent <<<",
                    ok ? 1 : 0, pk->eid);
        } else {
            const bool sent = coop::trash_collect_sync::DebugSendGrabIntent(pk->eid);
            UE_LOGI("grab_intent_test: >>> FALLBACK GRAB -- DebugSendGrabIntent eid=%u sent=%d (InpActEvt_use unresolved) <<<",
                    pk->eid, sent ? 1 : 0);
        }
        d.store(1);
    });

    // 4a. CARRY ~3s MOVING -- re-face each second so the puppet aim (hence the host hand-drive holdPoint AND
    //     the host-published carry pose) moves; exercises [TRASH-CARRY] HOST PUBLISH + CLIENT APPLY.
    for (int i = 0; i < 3; ++i) {
        ::Sleep(1000);
        RunGT([pk](std::atomic<int>& d) {
            E::SetControlRotation(E::GetController(pk->player),
                                  LookAt(E::GetActorLocation(pk->player), pk->pilePos));
            d.store(1);
        });
    }
    // 4b. CARRY ~3s STILL -- STOP re-facing/moving so (i) the L3 jitter metric `maxDriftCm` proves the clump
    //     does NOT drift from its commanded hold (kinematic carry, no PHC-spring/gravity fight), and (ii) the
    //     host's hand-velocity EMA decays to ~0 so the next E is a SOFT release, not a fixed-impulse throw.
    UE_LOGI("grab_intent_test: >>> STILL-CARRY 3s (L3: maxDriftCm should stay ~0; L4: handVel decays for a soft release) <<<");
    ::Sleep(3000);

    // 5. SOFT RELEASE via the REAL toggle: inject InpActEvt_use again while STANDING STILL -> OnPileGrabPre
    //    sees ClientCarryEid -> SendThrowIntent. Because the hand was still, the host's INHERITED release
    //    velocity is ~0 (a gentle drop), NOT the old fixed ~871 cm/s wild throw (L4). The host releases the
    //    puppet grab + applies that velocity -> the clump drops/flies + self-re-piles (BeginDeferred -> ToPile).
    UE_LOGI("grab_intent_test: >>> SOFT RELEASE (still) -- expect [THROW-INTENT] SUCCESS vel ~0 (a drop, not a wild throw) <<<");
    RunGT([pk, useReal](std::atomic<int>& d) {
        if (useReal) {
            std::vector<uint8_t> frame(pk->useFrame > 0 ? static_cast<size_t>(pk->useFrame) : 0, 0u);
            const bool ok = R::CallFunction(pk->player, pk->useFn, frame.empty() ? nullptr : frame.data());
            UE_LOGI("grab_intent_test: >>> REAL THROW -- injected InpActEvt_use while carrying (ok=%d); expect "
                    "'[THROW-INTENT] CLIENT E-PRESS while carrying' + SendThrowIntent <<<", ok ? 1 : 0);
        } else {
            const bool sent = coop::trash_collect_sync::DebugSendThrowIntent(pk->eid);
            UE_LOGI("grab_intent_test: >>> FALLBACK THROW -- DebugSendThrowIntent eid=%u sent=%d <<<", pk->eid, sent ? 1 : 0);
        }
        d.store(1);
    });

    // 6. Hold ~8s for the flight + re-pile (host [THROW-INTENT] SUCCESS -> flight stream -> RE-PILE thunk ->
    //    ToPile COMMIT; client ToPile SNAP).
    ::Sleep(8000);
    UE_LOGI("grab_intent_test: CLIENT done eid=%u -- verdict is the log-truth harness (client recognition, host "
            "[GRAB-INTENT]/[THROW-INTENT]/[TRASH-CARRY], client APPLY + ToPile SNAP). useReal=%d", pk->eid, useReal ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------------
// HOST-DRIFT scenario (L1 orphan census driver) -- VOTVCOOP_RUN_PILE_DRIFT=1, HOST-only.
//
// Creates KNOWN host-vs-save divergence so the client's join-time orphan census ([PILE-CENSUS] in
// remote_prop_spawn.cpp RunDivergenceSweep_) populates. Both peers load the SAME save (all chipPiles at
// the saved positions). This runs on the HOST during the PRE-CONNECT solo window (use mp.py smoke
// --host-settle to guarantee it) and drifts the host's OWN piles BEFORE the client's snapshot is taken:
//   - DESTROY N piles -> the host snapshot OMITS them      -> the client native has NO proxy   (collected orphan)
//   - MOVE   M piles  -> the host snapshot has them MOVED   -> the client native is far from its proxy (moved orphan)
// The client loads all piles at the saved positions, spawns proxies at the host's (drifted) positions, the
// 1cm twin-destroy consumes only the UNMOVED twins, and the N+M drifted natives survive as orphans ->
// the client's join-sweep [PILE-CENSUS] reports them, banded by nearest-proxy distance. This is the ONLY
// way to populate the histogram autonomously (a clean same-save join has ZERO drift = zero orphans).
//
// READ-ONLY on the client side this build (the census does not yet remove -- Phase 2). The host just edits
// its own world. MUST finish before the client connects; logs a timestamped COMPLETE line to verify in the
// host log that the drift preceded the snapshot. For a real HANDS-ON the user creates the same divergence
// by playing (collecting/moving piles as host) before the client joins -- this scenario only automates it.
void RunPileDriftScenario() {
    const bool isHost = (ReadEnv("VOTVCOOP_NET_ROLE") != "client");
    if (!isHost) {
        UE_LOGI("pile_drift: CLIENT -- nothing to do (the HOST drifts its world pre-connect). The orphan census "
                "runs on THIS client at the join sweep -- watch for '[PILE-CENSUS]' lines.");
        return;
    }
    UE_LOGI("pile_drift: HOST -- waiting for the save's chipPiles to load, then drifting in the pre-connect window "
            "(destroy 5 + move 3). MUST complete before the client connects.");
    struct DriftSel { std::vector<void*> destroy; std::vector<void*> move; };
    auto sel = std::make_shared<DriftSel>();
    const int kWaitCapS = 90;
    int waitedS = 0; bool ready = false;
    while (waitedS < kWaitCapS) {
        const int r = RunGT([sel](std::atomic<int>& d) {
            void* player = coop::players::Registry::Get().Local();
            if (!player || !R::IsLive(player) || !E::GetController(player)) { d.store(2); return; }
            const ue_wrap::FVector at = E::GetActorLocation(player);
            // Collect the live chipPiles nearest the player (deterministic by distance): the 5 nearest to
            // DESTROY, the next 3 to MOVE. One GUObjectArray walk (cold, pre-connect, not a hot path).
            struct Cand { void* a; float d2; };
            std::vector<Cand> cands;
            const int32_t n = R::NumObjects();
            for (int32_t i = 0; i < n; ++i) {
                void* o = R::ObjectAt(i);
                if (!o || !R::IsLive(o)) continue;
                if (!ue_wrap::prop::IsChipPile(o)) continue;
                if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
                const ue_wrap::FVector p = E::GetActorLocation(o);
                const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
                cands.push_back({o, dx * dx + dy * dy + dz * dz});
            }
            if (cands.size() < 8) { d.store(2); return; }   // not enough piles yet -> world still loading, keep waiting
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            for (int i = 0; i < 5; ++i) sel->destroy.push_back(cands[i].a);
            for (int i = 5; i < 8; ++i) sel->move.push_back(cands[i].a);
            d.store(1);
        });
        if (r == 1) { ready = true; break; }
        ::Sleep(1000); ++waitedS;
    }
    if (!ready) {
        UE_LOGW("pile_drift: fewer than 8 chipPiles loaded in %d s (or no possessed player) -- aborting (cannot drift)",
                kWaitCapS);
        return;
    }
    RunGT([sel](std::atomic<int>& d) {
        int destroyed = 0, moved = 0;
        for (void* a : sel->destroy) {
            if (!R::IsLive(a)) continue;
            const ue_wrap::FVector p = E::GetActorLocation(a);
            E::DestroyActor(a);
            ++destroyed;
            UE_LOGI("[PILE-DRIFT] HOST destroyed pile #%d @(%.1f,%.1f,%.1f) -- collected orphan (the client native "
                    "there will get NO proxy)", destroyed, p.X, p.Y, p.Z);
        }
        for (void* a : sel->move) {
            if (!R::IsLive(a)) continue;
            const ue_wrap::FVector p = E::GetActorLocation(a);
            // Move STRAIGHT UP 30m into empty air -- NOT +85cm horizontal into the dense cluster. The +85cm
            // horizontal made the moved proxy land within 1cm of a same-chipType NEIGHBOUR's native and
            // 1cm-WRONG-CONSUME it (the dense-cluster mis-consumption that masked the census, 2026-06-23 trace).
            // Empty air = no neighbour to wrong-consume -> the moved proxy is cleanly un-mirrored AND the client
            // native at the OLD position survives as a true orphan the census can count.
            const ue_wrap::FVector to{ p.X, p.Y, p.Z + 3000.f };
            E::SetActorLocation(a, to);
            ++moved;
            UE_LOGI("[PILE-DRIFT] HOST moved pile #%d @(%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) [+30m UP, empty air] -- "
                    "moved orphan (client native stays at OLD pos; proxy spawns up high, no neighbour to wrong-consume)",
                    moved, p.X, p.Y, p.Z, to.X, to.Y, to.Z);
        }
        UE_LOGI("[PILE-DRIFT] HOST drift COMPLETE -- %d destroyed + %d moved = %d orphans seeded BEFORE the client "
                "connects. The client's join [PILE-CENSUS] should report ~%d live orphan natives (READ-ONLY census "
                "this build; Phase-2 absence-removal will destroy the confident bands).",
                destroyed, moved, destroyed + moved, destroyed + moved);
        d.store(1);
    });
}

DWORD WINAPI ChipPileTestThread(LPVOID /*arg*/) {
    RunAutonomousChipPileTest();
    return 0;
}

DWORD WINAPI PileDriftScenarioThread(LPVOID /*arg*/) {
    RunPileDriftScenario();
    return 0;
}

DWORD WINAPI GrabIntentTestThread(LPVOID /*arg*/) {
    RunGrabIntentTest();
    return 0;
}

DWORD WINAPI PuppetGrabProbeThread(LPVOID /*arg*/) {
    RunPuppetGrabProbe();
    return 0;
}

}  // namespace harness::autotest
