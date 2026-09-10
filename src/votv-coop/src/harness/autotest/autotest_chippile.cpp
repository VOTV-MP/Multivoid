// harness/autotest/autotest_chippile.cpp -- the autonomous chipPile scenarios: the host grab, carry
// and throw (VOTVCOOP_RUN_CHIPPILE_TEST=1), the puppet-grab probe
// (VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1), the synthetic grab-intent test
// (VOTVCOOP_RUN_GRAB_INTENT_TEST=1) and the host-drift scenario (VOTVCOOP_RUN_PILE_DRIFT=1). The
// grab rides the game's own path: the E-press UFunction the input system dispatches, so the PRE
// observer fires as for a real player, and the pile's own playerGrabbed verb. The verdicts are read
// off the logs by the log-assert harness. VOTVCOOP_PILE_SHOWCASE=1 additionally aims the client
// camera at a mirrored pile and holds.

#include "harness/autotest.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"        // RemotePlayer::GetActor (client-in-world readiness gate)
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_collect_sync.h"   // DebugSendGrabIntent, DebugSendThrowIntent
#include "coop/props/trash_proxy.h"          // NearestPileProxy
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

// The look rotation from `from` toward `to` (Yaw about Z, Pitch about Y), the forward-vector
// convention autotest.cpp uses.
ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail); engine state is
// game-thread only.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// The eid bound to a host pile (the forward map) or a mirror pile (the wire); kInvalidId when
// untracked.
coop::element::ElementId EidOf(void* pile) {
    coop::element::ElementId e = PT::GetPropElementIdForActor(pile);
    if (e == coop::element::kInvalidId) e = coop::remote_prop::ResolveMirrorEidByActor(pile);
    return e;
}

}  // namespace

void RunAutonomousChipPileTest() {
    const bool isHost = !IsClientRole();
    if (!isHost) {
        if (ReadEnv("VOTVCOOP_PILE_SHOWCASE") != "1") {
            UE_LOGI("chippile_test: CLIENT scan-only -- verify in THIS log: 'remote_prop::OnConvert: "
                    "eid=N re-skin -> clump' (the carry mirrored), then '-> pile' (the re-pile). The "
                    "host drives the grab; the convert arrives over the wire.");
            return;
        }
        // The showcase: the client waits for the host's pile work and its proxies, teleports to a
        // standoff facing the nearest pile proxy and holds, re-facing each second, so a window
        // capture shows a mirrored pile.
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

    // The settle gates the grab on a signal the host can observe: the client's puppet going live
    // (spawned once the client connects and handshakes), then a margin for the client to receive
    // and express the host's prop snapshot. A fixed sleep once grabbed before the client had
    // expressed its proxies, and the convert had nothing to re-skin.
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

    // 1. The local player and the E-press UFunction.
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

    // 2. An eid-tracked chipPile (the eid addresses the peer's mirror): the nearest, else the
    // nearest tracked one.
    struct PileSel { void* pile = nullptr; ue_wrap::FVector pos{}; uint32_t eid = 0; float dist = 0.f; bool tracked = false; };
    auto sel = std::make_shared<PileSel>();
    if (RunGT([rsv, sel](std::atomic<int>& d) {
            // Every prop is force-tracked now (eids assigned, the actor-to-eid map filled) rather
            // than waiting for the reconcile; ReSeed is idempotent.
            const size_t reseeded = PT::ReSeedKnownKeyedProps(nullptr);
            UE_LOGI("chippile_test: forced ReSeedKnownKeyedProps -> %zu tracked (pile eids now bound)", reseeded);
            const ue_wrap::FVector at = E::GetActorLocation(rsv->player);
            float dist = -1.f;
            void* nearest = ue_wrap::prop::FindNearestChipPile(at, /*radiusCm=*/200000.f, &dist);
            if (!nearest) { UE_LOGW("chippile_test: NO chipPile in the world -- the save has none to grab"); d.store(2); return; }
            void* pick = nearest; float pickDist = dist;
            coop::element::ElementId eid = EidOf(nearest);
            if (eid == coop::element::kInvalidId) {
                // The nearest is untracked: scan for the nearest tracked one, keeping the untracked
                // nearest as the fallback.
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

    // 3. Teleport the player to a standoff in front of the pile, approaching from where it stands
    // (open space, not the wall a pile often sits against), then let the character settle.
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

    // 4. Face the pile. Not gated on the trace: the grab below injects lookAtActor for its one
    // dispatch, and the clump spawns at the pile's transform, so the camera need not point at it.
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

    // 5. The grab, two calls in one game-thread dispatch. A reflection InpActEvt_use call fires the
    // PRE observer but not the BP grab body (the input system drives the ubergraph, not the stub),
    // so 5a arms the observer the production way (lookAtActor injected, then the UFunction) and 5b
    // runs the pile's own playerGrabbed, the full body: the clump spawn, pickupObjectDirect, then
    // K2_DestroyActor on the pile itself. The pile is never touched after the call.
    UE_LOGI("chippile_test: >>> GRAB: arm(InpActEvt_use) + run real conversion(playerGrabbed) eid=%u <<<", sel->eid);
    RunGT([rsv, sel](std::atomic<int>& d) {
        // 5a: arm.
        E::WriteMainPlayerLookAtActor(rsv->player, sel->pile);
        std::vector<uint8_t> frame(rsv->useFrame > 0 ? static_cast<size_t>(rsv->useFrame) : 0, 0u);
        const bool armOk = R::CallFunction(rsv->player, rsv->useFn, frame.empty() ? nullptr : frame.data());
        // 5b: the real playerGrabbed(mainPlayer, HitResult{0}) on the pile.
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

    // 6. The field-routing probe: which field the spawned clump lands in. holding_actor means the
    // held-object channel sees it; grabbing_actor means it rode the physics-handle path.
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
                // The grab state is logged on the first four polls and on any clump edge.
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
    // The measured answer: the clump rides grabbing_actor (the physics-handle path), which
    // local_streams reads first, holding_actor being its fallback.
    UE_LOGI("chippile_test: FIELD-ROUTING VERDICT -- clump-in-grabbing_actor=%d clump-in-holding_actor=%d -- %s",
            sawClumpGrabbing ? 1 : 0, sawClumpHolding ? 1 : 0,
            sawClumpGrabbing ? "clump rides grabbing_actor (PHC path); local_streams reads it first so the morph adopts it -- CORRECTED premise, morph OK"
          : sawClumpHolding  ? "clump rides holding_actor (the old assumed field)"
                             : "NEITHER -- no clump was held (playerGrabbed did not run, or the clump self-freed before the first poll)");

    // 6.5. A sustained moving carry: the probe above is stationary, so a client drive that never
    // followed would pass unnoticed. The host walks about 8 s with the clump held, in small steps
    // so the physics handle keeps it in hand, re-confirming the hold each step; the client log is
    // judged for the carry drive advancing.
    if (heldClump) {
        ue_wrap::FVector base{};
        RunGT([rsv, &base](std::atomic<int>& d) { base = E::GetActorLocation(rsv->player); d.store(1); });
        float dx = base.X - sel->pos.X, dy = base.Y - sel->pos.Y;   // carry AWAY from the pile origin (open space)
        const float h = std::sqrt(dx * dx + dy * dy);
        if (h < 1.f) { dx = 1.f; dy = 0.f; } else { dx /= h; dy /= h; }
        // 15 cm per 100 ms, a brisk walk; a 4 m/s step imparted that speed to the held clump and
        // broke the grab mid-carry.
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

    // 7. The re-pile, best-effort. throwHoldingProp through reflection does not release a
    // physics-handle grab, so the handle is released cleanly and the freed clump thrown; its impact
    // re-piles it, and the land watch converts the peer's mirror back.
    if (heldClump) {
        RunGT([rsv, heldClump](std::atomic<int>& d) {
            const bool rel = E::ReleaseMainPlayerGrabIfHolding(rsv->player, heldClump);
            // A directional throw rather than a drop, so the flight stream carries a real arc:
            // toward the held clump, about 6 m/s forward and 4 m/s up.
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

    UE_LOGI("chippile_test: DONE -- grab-clump-holding=%d eid=%u. VERDICT is now the log-assert harness: "
            "host '[PILE] HOST GRAB ADOPT' + '[TRASH-CH] HOST carry OPEN' + "
            "'HOST RE-PILE(thunk)' + 'LAND COMMIT'; CLIENT '[PILE] CLIENT recv convert GRAB/LAND -> PROXY "
            "re-skinned' + 'CLIENT ToPile SNAP ... drift=~0'. (The old 'pile_morph: grab armed/ADOPTED' "
            "markers are RETIRED -- the morph was replaced by the host-auth trash channel + proxy.)",
            sawClumpHolding ? 1 : 0, sel->eid);
}

// The puppet-grab probe, host only (VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1). The host runs the real grab
// verb on a peer's puppet (an unpossessed mainPlayer_C) through playerGrabbed; the grab path
// touches no controller state, so it should engage. What bytecode cannot show is whether an
// unpossessed puppet's tick runs the per-tick physics-handle maintenance, so the clump tracks to
// the hand rather than floating at the spawn spot. Verdicts, asserted by the log-assert harness:
// ENGAGED (grabbing_actor became a clump), HELD (it stayed), TRACKED (the clump was pulled to the
// hand: the tick runs), FLOATING (held but not tracked: the tick is suppressed, and the hand must
// be driven from the synced aim).
void RunPuppetGrabProbe() {
    const bool isHost = !IsClientRole();
    if (!isHost) {
        UE_LOGI("puppet_grab_probe: CLIENT -- nothing to drive (the HOST executes the grab on the puppet). "
                "Just stand so the slot-1 puppet exists on the host.");
        return;
    }

    // 1. Wait for the slot-1 puppet to go live in the host's world.
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
    // A null controller is the local-versus-puppet discriminator; an actor with one is not a
    // puppet, so the probe aborts rather than report a false pass.
    if (pup->hasController) {
        UE_LOGW("puppet_grab_probe: slot-1 actor HAS a controller -- that is NOT a puppet; aborting (mis-wired)");
        return;
    }
    UE_LOGI("puppet_grab_probe: slot-1 puppet LIVE actor=%p, GetController()==null CONFIRMED (a true puppet). "
            "+20 s margin for the world to settle, then the puppet grab", pup->actor);
    ::Sleep(20000);

    // 2. The nearest live chipPile to the puppet; tracking is irrelevant, since this probes the raw
    // hold, not the wire.
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

    // 3. The puppet grab: the pile's own playerGrabbed with the puppet as the player. It spawns the
    // clump, sets its holder and calls the puppet's pickupObjectDirect; the pile self-destructs. No
    // observer arming, no lookAtActor injection.
    UE_LOGI("puppet_grab_probe: >>> executing playerGrabbed on the PUPPET (the host-side move) <<<");
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

    // 4. Poll the grab state and geometry for about 4 s: the horizontal puppet-to-clump distance,
    // the clump's height over the puppet (it rises to hand height when pulled up) and the grab
    // length.
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

    // 5. The verdict. ENGAGED if any poll saw a held clump; HELD if at least 70% did; TRACKED if
    // the clump ended within 250 cm and near hand height. Every number is logged, so the truth is
    // readable where the heuristic is borderline.
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
                "per-tick PHC maintenance RUNS on the unpossessed puppet. A host-side grab works as-is "
                "(verdict A); the only remaining input is syncing the puppet aim, which the mod already streams.");
    else
        UE_LOGI("puppet_grab_probe: RESULT = FLOATING (held, tick likely DEAD) -- the clump did not reach the "
                "puppet's hand (dist/last=%.0f cm, dZ=%.0f cm). The host must drive SetTargetLocationAndRotation "
                "on the puppet each tick from the synced remote aim (verdict B-fallback).", distLast, dzLast);
}

// The synthetic grab-intent test (VOTVCOOP_RUN_GRAB_INTENT_TEST=1). The client picks a mirrored
// pile proxy, resolves its host eid and drives the client-to-host path: the E-press observer's
// camera-ray recognition and its GrabIntent, the router, the host's validation and
// playerGrabbed on the puppet, the convert broadcast and the puppet hand drive. The client
// drives; the host executes and broadcasts. The verdict is the log harness.
void RunGrabIntentTest() {
    const bool isHost = !IsClientRole();
    if (isHost) {
        UE_LOGI("grab_intent_test: HOST -- the authority. Watch THIS log for the grab-intent RECEIVED/EXEC/"
                "SUCCESS + puppet-drive markers when the client sends its synthetic grab.");
        return;
    }

    // 1. Wait for the client to be in-world with its pile proxies expressed.
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
            // The E-press UFunction, so the real recognition path runs rather than only the debug
            // bypass.
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* fn  = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
            pk->player = p; pk->pile = pile; pk->eid = static_cast<uint32_t>(eid);
            pk->pilePos = E::GetActorLocation(pile); pk->dist = dist;
            pk->useFn = fn; pk->useFrame = fn ? R::FunctionFrameSize(fn) : 0;
            UE_LOGI("grab_intent_test: picked pile proxy=%p eid=%u pos=(%.0f,%.0f,%.0f) dist=%.0fcm useFn=%p",
                    pile, pk->eid, pk->pilePos.X, pk->pilePos.Y, pk->pilePos.Z, dist, fn);
            d.store(1);
        }) != 1) { UE_LOGW("grab_intent_test: could not pick a pile -- aborting"); return; }

    // 2. Teleport the client to a standoff facing the proxy, so its trace can hit it and its puppet
    // stands at the pile.
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

    // 3. The grab through the real path: InpActEvt_use injected, the observer's camera-ray cone
    // recognises the aimed proxy and sends the intent. The debug bypass runs only if the UFunction
    // did not resolve.
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

    // 4a. Carry about 3 s moving, re-facing each second so the puppet aim and the published carry
    // pose move.
    for (int i = 0; i < 3; ++i) {
        ::Sleep(1000);
        RunGT([pk](std::atomic<int>& d) {
            E::SetControlRotation(E::GetController(pk->player),
                                  LookAt(E::GetActorLocation(pk->player), pk->pilePos));
            d.store(1);
        });
    }
    // 4b. Carry about 3 s still, so the drift metric shows the clump holding its commanded pose and
    // the host's hand-velocity average decays, making the next press a soft release.
    UE_LOGI("grab_intent_test: >>> STILL-CARRY 3s (L3: maxDriftCm should stay ~0; L4: handVel decays for a soft release) <<<");
    ::Sleep(3000);

    // 5. The soft release through the real toggle: InpActEvt_use again while still, so the observer
    // sends the throw intent and the host's inherited release velocity is near zero (a drop, not a
    // throw).
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

    // 6. Hold about 8 s for the flight and the re-pile.
    ::Sleep(8000);
    UE_LOGI("grab_intent_test: CLIENT done eid=%u -- verdict is the log-truth harness (client recognition, host "
            "[GRAB-INTENT]/[THROW-INTENT]/[TRASH-CARRY], client APPLY + ToPile SNAP). useReal=%d", pk->eid, useReal ? 1 : 0);
}

// The host-drift scenario (VOTVCOOP_RUN_PILE_DRIFT=1), host only: known host-versus-save
// divergence, so the client's join-time orphan census has something to count. Both peers load
// the same save; in the pre-connect window the host destroys five piles (the snapshot omits
// them, so the client's natives get no proxy) and moves three (the client's natives sit far
// from their proxies). It must finish before the client connects, and logs a timestamped
// completion line to check that against the host log.
void RunPileDriftScenario() {
    const bool isHost = !IsClientRole();
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
            // The live chipPiles nearest the player, by distance: five to destroy, the next three
            // to move. One cold walk.
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
            // Moved 30 m straight up into empty air: a short horizontal move landed the proxy
            // within a centimetre of a neighbour's native, which the twin match consumed instead.
            // In empty air the moved proxy is cleanly unmatched, and the native at the old position
            // survives as a true orphan.
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
