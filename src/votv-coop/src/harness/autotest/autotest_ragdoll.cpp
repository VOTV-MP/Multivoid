// harness/autotest/autotest_ragdoll.cpp -- the ragdoll and faint sync end-to-end autotest
// (VOTVCOOP_RUN_RAGDOLL_TEST). The client (the driver) calls the game's own ragdoll verb on
// its local possessed player (exactly what the faint key and the exhaustion faint do), waits,
// then the get-up verb, so its own ragdoll flag flips on and off; the host (the observer)
// polls its puppet for the client and confirms the ragdoll display appears and then goes,
// driven purely by the client's pose stream carrying the ragdoll state bit and the remote
// player's reconcile applying the verbs on the puppet. Reads and drives go through the engine
// wrappers (the shipping code path), so the test validates the accessors too. Game-thread
// work is posted; a bounded wait guards every wait so a faulting call cannot hang the smoke.
// Gated by the env var (registered in the dispatch; the smoke inherits it). The role is read
// from VOTVCOOP_NET_ROLE: client is the driver, anything else the host observer.

#include "harness/autotest.h"

#include "coop/config/config.h"

#include "coop/player/puppet_drive.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace cfg = coop::config;

// A bounded spin-wait on a game-thread task's completion flag. True if the task signalled,
// false if it never completed within the timeout, which means the posted task faulted (the
// SEH firewall ate the fault and the flag was never set). The bound is mandatory: driving the
// ragdoll verb on the local player could fault, and an unbounded wait would hang the whole
// smoke.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// Whether the slot-1 puppet has a live ragdoll display body whose mesh is physically
// simulating (the visible flop is a separate ragdoll body, not the puppet's own mesh). The
// rigid-body-awake query on the body's mesh confirms it is actually flopping: true while
// falling, false once settled. False on a missing or dead body.
bool PuppetHasFloppingRagdollBody() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto ok = std::make_shared<int>(0);
    GT::Post([done, ok] {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
        void* body = rp.RagdollBody();
        if (body && R::IsLiveByIndex(body, rp.RagdollBodyIdx())) {  // recycle-proof
            // The ragdoll body's mesh component.
            void* mesh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + 0x0230);
            if (mesh && R::IsLive(mesh)) {
                if (void* awakeFn = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"IsAnyRigidBodyAwake")) {
                    ue_wrap::ParamFrame f(awakeFn);
                    if (ue_wrap::Call(mesh, f) && f.Get<bool>(L"ReturnValue")) *ok = 1;
                }
            }
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    return *ok != 0;
}

// Aim the host player's camera at the slot-1 puppet's body, so an autonomous screenshot
// frames the falling puppet. The puppet converges next to the host but often off to the side
// or behind, out of the forward view. Computes the look-at from the host camera (the actor
// plus eye height) to the puppet's mesh location (the limp body during the flop) and writes
// the host controller's control rotation. Game thread; bounded.
void AimHostAtPuppet() {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        void* local = coop::players::Registry::Get().Local();
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        if (!local || !R::IsLive(local) || !puppet || !R::IsLive(puppet)) { done->store(1); return; }
        void* ctrl = E::GetController(local);
        if (!ctrl || !R::IsLive(ctrl)) { done->store(1); return; }
        const ue_wrap::FVector h = E::GetActorLocation(local);
        void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
        const ue_wrap::FVector p = (mesh && R::IsLive(mesh)) ? E::GetComponentLocation(mesh)
                                                             : E::GetActorLocation(puppet);
        const float dx = p.X - h.X, dy = p.Y - h.Y, dz = p.Z - (h.Z + 60.f);
        const float horiz = std::sqrt(dx * dx + dy * dy);
        const float yaw = std::atan2(dy, dx) * 57.29578f;
        const float pitch = std::atan2(dz, horiz) * 57.29578f;
        E::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, yaw, 0.f});
        UE_LOGI("ragdoll_test[host]: aimed host camera at puppet body (yaw=%.0f pitch=%.0f dist=%.0f)",
                yaw, pitch, std::sqrt(dx * dx + dy * dy + dz * dz));
        done->store(1);
    });
    WaitDone(done, 8000);
}

// Move the host back from the puppet (the same Z, so it stays on the floor) so the fallen
// body is ahead in the frame, not directly under the host's own first-person legs (the host
// and client spawn overlapping, so a straight-down view is just the host's own feet occluding
// the body). One shot at the rising edge.
void PositionHostForShot() {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        void* local = coop::players::Registry::Get().Local();
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        if (!local || !R::IsLive(local) || !puppet || !R::IsLive(puppet)) { done->store(1); return; }
        const ue_wrap::FVector h = E::GetActorLocation(local);
        const ue_wrap::FVector p = E::GetActorLocation(puppet);
        float dx = h.X - p.X, dy = h.Y - p.Y;            // direction AWAY from the puppet
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.f) { dx = 1.f; dy = 0.f; } else { dx /= len; dy /= len; }
        const ue_wrap::FVector dst{ p.X + dx * 280.f, p.Y + dy * 280.f, h.Z };
        E::SetActorLocation(local, dst);
        UE_LOGI("ragdoll_test[host]: moved host to (%.0f,%.0f,%.0f) ~280u back from puppet for the shot",
                dst.X, dst.Y, dst.Z);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// The geometry probe: samples whether the body's bounds and lowest bone actually drop toward
// the host's feet during the flop (a real fall) or stay pinned at chest height (a welded
// root: the visible mesh simulated without detaching from its parent, so the rig cannot
// translate to the floor). Read-only. Game thread.
void ProbePuppetRagdollGeometry(const char* tag) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, tag] {
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        void* local = coop::players::Registry::Get().Local();
        if (!puppet || !R::IsLive(puppet)) { done->store(1); return; }
        ue_wrap::FVector origin{}, extent{};
        const bool okB = E::GetActorBounds(puppet, false, origin, extent);
        const float bottom = origin.Z - extent.Z;
        void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
        const float compZ = (mesh && R::IsLive(mesh)) ? E::GetComponentLocation(mesh).Z : 0.f;
        float lowBoneZ = 0.f;
        const bool okBone = (mesh && R::IsLive(mesh)) ? E::GetLowestBoneWorldZ(mesh, lowBoneZ) : false;
        const float hostZ = (local && R::IsLive(local)) ? E::GetActorLocation(local).Z : 0.f;
        UE_LOGI("[probe ragdoll-geom %s]: bounds origin.Z=%.0f bottom=%.0f(ok=%d) compZ=%.0f "
                "lowestBoneZ=%.0f(ok=%d) hostActorZ=%.0f -> floor~%.0f (body FALLS if bottom/bone "
                "approach the floor; WELDED if they stay ~compZ chest height)",
                tag, origin.Z, bottom, okB ? 1 : 0, compZ, lowBoneZ, okBone ? 1 : 0,
                hostZ, hostZ - 88.f);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// The host side: observe the slot-1 puppet (the client's body) and confirm its ragdoll
// appears (driven by the client over the wire) and then goes.
void ObserveOnHost() {
    UE_LOGI("ragdoll_test[host]: observer armed -- polling slot-1 puppet for a "
            "wire-driven ragdoll (up to 120 s)");

    // The phase machine: 0 waiting for the puppet to exist; 1 the puppet up, settle and frame
    // the standing puppet (the before shot), then wait for the rising edge; 2 waiting for the
    // falling edge.
    int phase = 0;
    bool sawPuppet = false;
    bool sawFlop = false;
    bool positioned = false;  // host moved back + aimed at the standing puppet
    int settle = 0;           // poll ticks waited for the puppet to converge
    for (int attempt = 0; attempt < 120 && phase < 3; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto isRag = std::make_shared<int>(-1);  // -1 = no puppet, 0 = up, 1 = ragdoll-displayed
        GT::Post([done, isRag] {
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
            if (!rp.valid()) { *isRag = -1; done->store(1); return; }
            // A spawned ragdoll display body is the authoritative sign that this puppet is
            // ragdolled; the puppet's own ragdoll field is not driven.
            *isRag = rp.IsRagdollDisplayed() ? 1 : 0;
            done->store(1);
        });
        WaitDone(done, 8000);
        const int r = *isRag;

        if (phase == 0) {
            if (r >= 0 && !sawPuppet) {
                sawPuppet = true;
                UE_LOGI("ragdoll_test[host]: slot-1 puppet resolved (isRagdoll=%d) -- waiting for wire-driven ragdoll", r);
            }
            if (r >= 0) phase = 1;  // puppet readable -> start watching for the rising edge
        }
        // Phase 1: the puppet is up. Settle (let it converge from the spawn placeholder), then move
        // the host back and aim at the standing puppet and announce the before shot is ready; then
        // keep tracking until the ragdoll fires.
        if (phase == 1 && !positioned) {
            if (++settle >= 4) {
                PositionHostForShot();
                AimHostAtPuppet();
                positioned = true;
                UE_LOGI("ragdoll_test[host]: host positioned + aimed at STANDING puppet -- BEFORE-SHOT READY");
                ProbePuppetRagdollGeometry("standing");  // baseline before the flop
            }
        } else if (phase == 1 && positioned) {
            AimHostAtPuppet();  // keep the standing puppet framed until it flops
            if (r == 1) {
                // Confirm the separate ragdoll display body is physically flopping (not just the
                // display latch): the rigid-body-awake query on the body mesh, true while it falls,
                // false once it settles.
                sawFlop = PuppetHasFloppingRagdollBody();
                UE_LOGI("ragdoll_test[host]: observed RISING edge -- puppet ragdoll-displayed over the "
                        "wire; separate playerRagdoll_C body physically flopping=%d", sawFlop ? 1 : 0);
                phase = 2;
            }
        }
        if (phase == 2 && r == 1) {
            AimHostAtPuppet();  // track the puppet (anchored over the flopping body)
        }
        if (phase == 2 && r == 0) {
            UE_LOGI("ragdoll_test[host]: observed FALLING edge -- puppet ragdoll body destroyed (recovered)");
            phase = 3;
        }
        ::Sleep(1000);
    }

    if (phase >= 3) {
        UE_LOGI("ragdoll_test[host]: VERDICT ragdoll e2e %s -- a client's local ragdoll "
                "propagated to its host puppet (rising + falling) purely via the pose stream; the puppet "
                "spawned a SEPARATE playerRagdoll_C body that physically flopped=%d (death-free, no host kill)",
                sawFlop ? "PASS" : "PARTIAL (no flop)", sawFlop ? 1 : 0);
    } else if (!sawPuppet) {
        UE_LOGW("ragdoll_test[host]: VERDICT INCONCLUSIVE -- slot-1 puppet never resolved "
                "(no client connected? puppet never spawned)");
    } else if (phase == 1) {
        UE_LOGW("ragdoll_test[host]: VERDICT FAIL -- puppet resolved but never saw a "
                "wire-driven ragdoll (rising edge). Sender bit or receiver reconcile broken.");
    } else {
        UE_LOGW("ragdoll_test[host]: VERDICT PARTIAL -- saw the ragdoll RISING edge but not "
                "the recover (falling). forceGetUp recover path may be broken.");
    }
    UE_LOGI("ragdoll_test[host]: DONE");
}

// The client side: drive the local possessed player into a ragdoll, then recover, the same
// verbs the faint key and the faint use. The local ragdoll flip rides the pose stream's
// ragdoll state bit to the host.
void DriveOnClient() {
    UE_LOGI("ragdoll_test[client]: driver armed -- waiting for the local player");

    // Wait for a live local player (post-possession). Poll up to 60 s.
    auto local = std::make_shared<void*>(nullptr);
    for (int attempt = 0; attempt < 60 && !*local; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, done] {
            void* mp = coop::players::Registry::Get().Local();
            if (mp && R::IsLive(mp)) *local = mp;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (!*local) ::Sleep(1000);
    }
    if (!*local) {
        UE_LOGW("ragdoll_test[client]: VERDICT INCONCLUSIVE -- no local player in 60 s; cannot drive");
        return;
    }

    // Let the host spawn the slot-1 puppet and arm its observer (the puppet appears a few
    // seconds after our first pose). Twelve seconds is comfortably past that on a LAN smoke.
    UE_LOGI("ragdoll_test[client]: local player resolved -- waiting 12 s for the host puppet to spawn");
    ::Sleep(12000);

    // Drive the local ragdoll (what the faint key and the exhaustion faint do). On the possessed
    // local player this is the normal in-game path.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        void* mp = *local;
        GT::Post([mp, done] {
            // The faithful driver: the game's real ragdoll verb on the local possessed player,
            // exactly what the faint key does, so the client ragdolls locally too and the test
            // matches real play. The flag rides the wire's ragdoll state bit to the host, which
            // attaches its puppet to a ragdoll body at the pelvis. The arguments: ragdoll, no faint
            // screen, no death.
            const bool ok = E::SetMainPlayerRagdollMode(mp, /*ragdoll=*/true, /*passOut=*/false, /*death=*/false);
            UE_LOGI("ragdoll_test[client]: ragdollMode(true,false,false) on the LOCAL player -> ok=%d "
                    "(real VOTV ragdoll -- client ragdolls locally; the flag rides the wire to the host puppet)", ok ? 1 : 0);
            done->store(1);
        });
        if (!WaitDone(done, 8000)) {
            UE_LOGW("ragdoll_test[client]: ragdollMode call did not complete (faulted?) -- aborting drive");
            return;
        }
    }

    // Hold the ragdoll. Five seconds by default (enough for the host observer's rising-edge
    // poll); VOTVCOOP_RAGDOLL_HOLD_MS overrides it, and the screenshot scenario sets a longer
    // hold so the orchestrator can capture the puppet while it is flopped (proving it falls
    // limp, not rigid).
    int holdMs = 5000;
    {
        const std::string h = cfg::ReadEnv("VOTVCOOP_RAGDOLL_HOLD_MS");
        if (!h.empty()) {
            const int v = std::atoi(h.c_str());
            if (v > 0 && v <= 120000) holdMs = v;
        }
    }
    UE_LOGI("ragdoll_test[client]: holding ragdoll for %d ms", holdMs);
    ::Sleep(holdMs);

    // Recover (the wake-up and get-up). Clears the local ragdoll animation gate; the cleared bit
    // rides the next pose so the host puppet gets up too.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        void* mp = *local;
        GT::Post([mp, done] {
            const bool ok = E::ForceMainPlayerGetUp(mp);
            UE_LOGI("ragdoll_test[client]: forceGetUp on the LOCAL player -> ok=%d (real recover; the cleared "
                    "isRagdoll rides the wire so the host puppet detaches + destroys its ragdoll body)", ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    ::Sleep(3000);
    UE_LOGI("ragdoll_test[client]: drive sequence DONE");
}

}  // namespace

void RunAutonomousRagdollTest() {
    const bool isHost = !IsClientRole();  // default Host if unset
    if (isHost) {
        ObserveOnHost();
    } else {
        DriveOnClient();
    }
}

DWORD WINAPI RagdollTestThread(LPVOID) {
    RunAutonomousRagdollTest();
    return 0;
}

}  // namespace harness::autotest
