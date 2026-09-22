// harness/autotest/autotest_grabintent.cpp -- the synthetic grab-intent test
// (VOTVCOOP_RUN_GRAB_INTENT_TEST=1): a client drives the trash carry round trip through the game's
// own E-press, and both logs carry the verdict (the trashcarry driver reads them).
// VOTVCOOP_GRAB_INTENT_HARD_THROW=1 adds a second leg, a throw up and away, and the client samples
// what its own mirror of the clump does in the air.

#include "harness/autotest.h"

#include "coop/dev/director/director.h"   // PickReachablePile, AddWalkToProcesses
#include "coop/player/players_registry.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_channel.h"        // ClientCarryEid (the fall leg reads the carry toggle)
#include "coop/props/trash_collect_sync.h"   // DebugSendGrabIntent, DebugSendThrowIntent, DebugSendHardThrowIntent
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"   // SetMainPlayerRagdollMode, ForceMainPlayerGetUp (the fall leg)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace harness::autotest {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// The look rotation from `from` toward `to` (Yaw about Z, Pitch about Y).
ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

}  // namespace

// The synthetic grab-intent test (VOTVCOOP_RUN_GRAB_INTENT_TEST=1). The client picks the nearest
// mirrored pile, resolves its host eid and drives the client-to-host path: the E-press
// interceptor's look-at recognition and its GrabIntent, the router, the host's validation and
// playerGrabbed on the puppet, the convert broadcast that materialises the clump mirror, the
// puppet hand drive, the throw intent and the landing convert back to a pile. The client drives;
// the host executes and broadcasts. The verdict is the log harness.
void RunGrabIntentTest() {
    const bool isHost = !IsClientRole();
    if (isHost) {
        UE_LOGI("grab_intent_test: HOST -- the authority. This log carries the receipt, the grab on the "
                "puppet and the carry publishes when the client sends its synthetic grab.");
        return;
    }

    // 1. Wait for the client to be in-world with its pile proxies expressed.
    UE_LOGI("grab_intent_test: CLIENT -- waiting 70s for the join and the pile binds, then picking a "
            "mirrored pile + sending GrabIntent");
    ::Sleep(70000);

    struct Pick { void* player = nullptr; void* pile = nullptr; uint32_t eid = 0; ue_wrap::FVector pilePos{};
                  uint32_t farEid = 0; float farCm = 0.f; void* useFn = nullptr; int32_t useFrame = 0; };
    auto pk   = std::make_shared<Pick>();
    auto goal = std::make_shared<coop::director::DirectorGoal>();
    if (RunGT([pk, goal](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (!p || !R::IsLive(p) || !E::GetController(p)) {
                UE_LOGW("grab_intent_test: no possessed local player"); d.store(2); return; }
            // The pile the client can WALK to: the director's pick, by NavMesh route. The nearest
            // pile in a straight line may sit behind a wall or on a shelf.
            if (!coop::director::PickReachablePile(p, 0.f, 5000.f, *goal)) {
                UE_LOGW("grab_intent_test: no nav-reachable mirrored pile"); d.store(2); return; }
            void* pile = goal->targetActor;
            coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(pile);
            if (eid == coop::element::kInvalidId) {
                UE_LOGW("grab_intent_test: the picked pile %p has no resolvable eid", pile); d.store(2); return; }
            // The refusal leg's target: the FARTHEST pile with an eid. Nobody moves for it.
            const ue_wrap::FVector at = E::GetActorLocation(p);
            for (void* o : R::FindObjectsByClass(L"actorChipPile_C")) {
                if (!o || !R::IsLive(o) || o == pile) continue;
                const ue_wrap::FVector op = E::GetActorLocation(o);
                const float dx = op.X - at.X, dy = op.Y - at.Y, dz = op.Z - at.Z;
                const float cm = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (cm <= pk->farCm) continue;
                const coop::element::ElementId fe = coop::remote_prop::ResolveMirrorEidByActor(o);
                if (fe == coop::element::kInvalidId) continue;
                pk->farCm = cm; pk->farEid = static_cast<uint32_t>(fe);
            }
            // The E-press UFunction, so the real recognition path runs rather than only the debug
            // bypass.
            void* cls = R::FindClass(P::name::MainPlayerClass);
            void* fn  = cls ? R::FindFunction(cls, P::name::MainPlayerUseInputEventFn) : nullptr;
            pk->player = p; pk->pile = pile; pk->eid = static_cast<uint32_t>(eid);
            pk->pilePos = goal->targetPos;
            pk->useFn = fn; pk->useFrame = fn ? R::FunctionFrameSize(fn) : 0;
            UE_LOGI("grab_intent_test: picked pile=%p eid=%u pos=(%.0f,%.0f,%.0f) useFn=%p; far pile eid=%u at %.0f cm",
                    pile, pk->eid, pk->pilePos.X, pk->pilePos.Y, pk->pilePos.Z, fn, pk->farEid, pk->farCm);
            d.store(1);
        }) != 1) { UE_LOGW("grab_intent_test: could not pick a pile -- aborting"); return; }

    // 1a. The refusal leg: a grab for a pile that is already far away, sent from where the client
    // stands. The host must refuse it for reach AND answer -- its DENIED line, then this client's
    // own 'grab REFUSED ... pending grab cleared'.
    if (pk->farEid != 0 && pk->farCm > 3000.f) {
        RunGT([pk](std::atomic<int>& d) {
            const bool sent = coop::trash_collect_sync::DebugSendGrabIntent(pk->farEid);
            UE_LOGI("grab_intent_test: >>> FAR GRAB eid=%u from %.0f cm sent=%d -- the host must refuse it and say so <<<",
                    pk->farEid, pk->farCm, sent ? 1 : 0);
            d.store(1);
        });
        ::Sleep(1500);
    } else {
        UE_LOGW("grab_intent_test: refusal leg SKIPPED -- no mirrored pile farther than 3000 cm (farthest %.0f)", pk->farCm);
    }

    // 2. WALK to the pile: the director's route over the NavMesh, driven at the input seam, so the
    // client arrives where a player can stand and the puppet on the host arrives with it.
    {
        goal->reachCm = 140.f;   // inside the game's own interaction trace
        coop::director::ControlManager mgr;
        coop::director::AddWalkToProcesses(mgr, *goal);
        mgr.Run(*goal, /*maxSeconds=*/225);
        if (!goal->reached) {
            UE_LOGW("grab_intent_test: the walk did not reach the pile (%s) -- aborting", goal->failReason);
            return;
        }
    }
    // Aim from the CAMERA, not from the capsule: the camera sits a head above the actor origin, so
    // a rotation computed at the origin points over the pile, and the trace misses.
    RunGT([pk](std::atomic<int>& d) {
        const ue_wrap::FRotator face = LookAt(E::GetCameraLocation(), pk->pilePos);
        E::SetControlRotation(E::GetController(pk->player), face);
        UE_LOGI("grab_intent_test: client walked to the pile and aimed from the camera; the look-at grab comes next");
        d.store(1);
    });
    ::Sleep(1500);   // let the view camera settle so the game's look-at trace names the pile

    // 2a. What the game's own trace says right before the press. The interceptor recognises the
    // grab through lookAtActor, so a press that does nothing is this line's to explain: a null or a
    // different actor is the recognition failing, not the intent path.
    RunGT([pk](std::atomic<int>& d) {
        void* aimed = E::ReadMainPlayerLookAtActor(pk->player);
        UE_LOGI("grab_intent_test: lookAtActor=%p class='%ls' isChipPile=%d target=%p (the interceptor reads this)",
                aimed, aimed ? R::ClassNameOf(aimed).c_str() : L"<null>",
                aimed && ue_wrap::prop::IsChipPile(aimed) ? 1 : 0, pk->pile);
        d.store(1);
    });

    // 3. The grab through the real path: InpActEvt_use injected, the interceptor recognises the
    // looked-at pile and sends the intent. The debug bypass runs only if the UFunction did not
    // resolve.
    const bool useReal = (pk->useFn != nullptr);
    RunGT([pk, useReal](std::atomic<int>& d) {
        if (useReal) {
            std::vector<uint8_t> frame(pk->useFrame > 0 ? static_cast<size_t>(pk->useFrame) : 0, 0u);
            const bool ok = R::CallFunction(pk->player, pk->useFn, frame.empty() ? nullptr : frame.data());
            UE_LOGI("grab_intent_test: >>> REAL GRAB -- injected InpActEvt_use (ok=%d) for eid=%u; the "
                    "interceptor's own press line and its intent follow <<<", ok ? 1 : 0, pk->eid);
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
    // 4b. Carry about 3 s still, so the lag metric shows the clump settled on the puppet's handle
    // and the body is at rest, making the next press a soft release: it leaves with what it has.
    UE_LOGI("grab_intent_test: >>> STILL-CARRY 3s (L3: maxLagCm stays a few cm on a free carry; L4: a still body is released with ~0 velocity, a soft release) <<<");
    ::Sleep(3000);

    // 5. The soft release through the real toggle: InpActEvt_use again while still, so the observer
    // sends the throw intent and the host's inherited release velocity is near zero (a drop, not a
    // throw).
    UE_LOGI("grab_intent_test: >>> SOFT RELEASE (still) -- the host's release velocity should be ~0, a drop "
            "rather than a wild throw <<<");
    RunGT([pk, useReal](std::atomic<int>& d) {
        if (useReal) {
            std::vector<uint8_t> frame(pk->useFrame > 0 ? static_cast<size_t>(pk->useFrame) : 0, 0u);
            const bool ok = R::CallFunction(pk->player, pk->useFn, frame.empty() ? nullptr : frame.data());
            UE_LOGI("grab_intent_test: >>> REAL THROW -- injected InpActEvt_use while carrying (ok=%d); the "
                    "interceptor's own carrying-press line and its intent follow <<<", ok ? 1 : 0);
        } else {
            const bool sent = coop::trash_collect_sync::DebugSendThrowIntent(pk->eid);
            UE_LOGI("grab_intent_test: >>> FALLBACK THROW -- DebugSendThrowIntent eid=%u sent=%d <<<", pk->eid, sent ? 1 : 0);
        }
        d.store(1);
    });

    // 5a. The hard throw (VOTVCOOP_GRAB_INTENT_HARD_THROW=1): the soft release above is a drop, and
    // a drop has no arc to look at. Re-grab the landed pile and throw it up and away, the LMB shape.
    size_t hardLen = 0; char hardBuf[8] = {};
    const bool hardThrow = ::getenv_s(&hardLen, hardBuf, sizeof(hardBuf), "VOTVCOOP_GRAB_INTENT_HARD_THROW") == 0 &&
                           hardLen > 1 && hardBuf[0] == '1';
    if (hardThrow) {
        ::Sleep(4000);   // the drop lands and converts back to a pile
        UE_LOGI("grab_intent_test: >>> HARD-THROW leg -- re-grab eid=%u, carry 2 s, throw up and away <<<", pk->eid);
        RunGT([pk](std::atomic<int>& d) { coop::trash_collect_sync::DebugSendGrabIntent(pk->eid); d.store(1); });
        ::Sleep(2000);
        RunGT([pk](std::atomic<int>& d) {
            const ue_wrap::FVector fwd = E::GetActorForwardVector(pk->player);
            const float k = 0.7071f;   // 45 degrees up along the facing
            const ue_wrap::FVector dir{ fwd.X * k, fwd.Y * k, k };
            const bool sent = coop::trash_collect_sync::DebugSendHardThrowIntent(pk->eid, dir);
            UE_LOGI("grab_intent_test: >>> HARD THROW eid=%u dir=(%.2f,%.2f,%.2f) sent=%d <<<",
                    pk->eid, dir.X, dir.Y, dir.Z, sent ? 1 : 0);
            d.store(1);
        });
    }

    // 6. The flight, SAMPLED on this client: what the mirror of the thrown clump does between the
    // throw and the landing is the measurement (a frozen mirror holds one position; an arc rises
    // and falls). 20 Hz for 4 s, then the rest of the hold for the re-pile.
    {
        const ULONGLONG t0 = ::GetTickCount64();
        for (int i = 0; i < 80; ++i) {
            RunGT([pk, t0](std::atomic<int>& d) {
                void* m = coop::remote_prop::ResolveLiveActorByEid(static_cast<coop::element::ElementId>(pk->eid));
                if (m) {
                    const ue_wrap::FVector at = E::GetActorLocation(m);
                    UE_LOGI("grab_intent_test: FLIGHT-SAMPLE t=%llu ms eid=%u mirror=%p class='%ls' pos=(%.1f,%.1f,%.1f)",
                            ::GetTickCount64() - t0, pk->eid, m, R::ClassNameOf(m).c_str(), at.X, at.Y, at.Z);
                } else {
                    UE_LOGI("grab_intent_test: FLIGHT-SAMPLE t=%llu ms eid=%u mirror=<none>",
                            ::GetTickCount64() - t0, pk->eid);
                }
                d.store(1);
            });
            ::Sleep(50);
        }
    }
    // 7. The fall (VOTVCOOP_GRAB_INTENT_FALL=1): the game drops what a fainting player holds, and
    // this player's clump is in its puppet's hand on the host. Re-grab the landed pile, carry, fall
    // with the game's own ragdoll verb: the host must let the clump go and say the carry is over.
    size_t fallLen = 0; char fallBuf[8] = {};
    const bool fall = ::getenv_s(&fallLen, fallBuf, sizeof(fallBuf), "VOTVCOOP_GRAB_INTENT_FALL") == 0 &&
                      fallLen > 1 && fallBuf[0] == '1';
    if (fall) {
        ::Sleep(4000);   // the last landing converts back to a pile
        UE_LOGI("grab_intent_test: >>> FALL leg -- re-grab eid=%u, carry 2 s, ragdoll <<<", pk->eid);
        RunGT([pk](std::atomic<int>& d) { coop::trash_collect_sync::DebugSendGrabIntent(pk->eid); d.store(1); });
        ::Sleep(2000);
        RunGT([pk](std::atomic<int>& d) {
            const bool ok = E::SetMainPlayerRagdollMode(pk->player, /*ragdoll=*/true, /*passOut=*/false, /*death=*/false);
            UE_LOGI("grab_intent_test: >>> FELL while carrying eid=%u (ragdollMode ok=%d) <<<", pk->eid, ok ? 1 : 0);
            d.store(1);
        });
        ::Sleep(4000);
        RunGT([pk](std::atomic<int>& d) {
            const bool ok = E::ForceMainPlayerGetUp(pk->player);
            UE_LOGI("grab_intent_test: >>> GOT UP (ok=%d); carrying now=%u (0 = the host ended the carry) <<<",
                    ok ? 1 : 0, static_cast<unsigned>(coop::trash_channel::ClientCarryEid() == coop::element::kInvalidId
                                                          ? 0u : coop::trash_channel::ClientCarryEid()));
            d.store(1);
        });
    }
    ::Sleep(4000);
    UE_LOGI("grab_intent_test: CLIENT done eid=%u -- the verdict is the round trip's own markers in both logs: "
            "the client's recognition and intents, the host's grab, carry and release, and the client's land. "
            "useReal=%d", pk->eid, useReal ? 1 : 0);
}

}  // namespace harness::autotest
